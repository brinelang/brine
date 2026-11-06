#!/bin/sh
# Builds brine from a released brine and packages it for this machine. CI and
# releases run it from the repository root; it works the same by hand.
#
#   sh scripts/dist.sh bootstrap   stage 0 -> target/boot/brine1 -> ./brine (the compiler under test)
#   sh scripts/dist.sh package     ./brine -> target/dist/brine-<os>-<arch>.tar.gz (.zip on Windows)
#
# Stage 0 is the release named in bootstrap/VERSION, downloaded and checked
# against bootstrap/SHA256SUMS, or $BRINE0 (a brine executable) when set. Needs
# curl and sha256sum or shasum, and a C compiler only if stage 0 needs one
# (0.8.0 links through cc: $CC, cc, gcc or clang).
set -eu

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    MINGW* | MSYS* | CYGWIN*) os=windows ;;
    *) echo "dist.sh: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) echo "dist.sh: unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
name="brine-$os-$arch"

exe=""
ext="tar.gz"
if [ "$os" = windows ]; then exe=".exe"; ext="zip"; fi

# The released brine that builds this one, downloaded once into target/boot.
stage0() {
    if [ -n "${BRINE0:-}" ]; then
        echo "$BRINE0"
        return
    fi
    v="$(cat bootstrap/VERSION)"
    d="target/boot/brine-$v"
    if [ ! -x "$d/$name/brine$exe" ]; then
        rm -rf "$d"
        mkdir -p "$d"
        echo "downloading brine $v ($name.$ext)" >&2
        curl -fsSL --retry 3 -o "$d/$name.$ext" "https://github.com/brinelang/brine/releases/download/v$v/$name.$ext"
        want="$(awk -v f="$name.$ext" '$2 == f { print $1 }' bootstrap/SHA256SUMS)"
        if command -v sha256sum > /dev/null; then
            got="$(sha256sum "$d/$name.$ext" | awk '{ print $1 }')"
        else
            got="$(shasum -a 256 "$d/$name.$ext" | awk '{ print $1 }')"
        fi
        if [ -z "$want" ] || [ "$got" != "$want" ]; then
            echo "dist.sh: $name.$ext doesn't match bootstrap/SHA256SUMS (expected '$want', got $got)" >&2
            exit 1
        fi
        if [ "$ext" = zip ]; then
            if command -v unzip > /dev/null; then
                (cd "$d" && unzip -q "$name.zip")
            else
                (cd "$d" && powershell.exe -NoProfile -Command "Expand-Archive -Path '$name.zip' -DestinationPath .")
            fi
        else
            tar -xzf "$d/$name.tar.gz" -C "$d"
        fi
    fi
    echo "$d/$name/brine$exe"
}

bootstrap() {
    b0="$(stage0)"
    "$b0" --version
    if [ -n "${CC:-}" ]; then export BRINE_CC="$CC"; fi
    # stage 1 carries stage 0's runtime and code generation; it rebuilds itself
    # so that the compiler under test has this checkout's
    "$b0" build compiler/main.br -o "target/boot/brine1$exe"
    "target/boot/brine1$exe" build compiler/main.br -o "brine$exe"
    "./brine$exe" --version
}

package() {
    [ -x "./brine$exe" ] || { echo "dist.sh: run 'sh scripts/dist.sh bootstrap' first" >&2; exit 1; }
    out="target/dist/$name"
    rm -rf target/dist
    mkdir -p "$out"
    # (built outside $out: a build leaves its cache stamp next to the executable)
    "./brine$exe" build --release compiler/main.br -o "target/boot/brine-release$exe"
    cp "target/boot/brine-release$exe" "$out/brine$exe"
    cp LICENSE README.md "$out/"

    # The packaged compiler must work on its own, outside the repository, and
    # without a C compiler (BRINE_CC points at none).
    smoke="$(mktemp -d)"
    cp "$out/brine$exe" "$smoke/"
    printf 'fn main():\n    print("hello from", par sum(i for i in 0..10))\n' > "$smoke/hello.br"
    (cd "$smoke" && export BRINE_CC=/no/cc && "./brine$exe" --version && "./brine$exe" run hello.br -q) > "$smoke/log" 2>&1 || {
        cat "$smoke/log" >&2
        exit 1
    }
    grep -q "hello from 45" "$smoke/log" || { cat "$smoke/log" >&2; exit 1; }
    rm -rf "$smoke"

    if [ "$os" = windows ]; then
        if command -v zip > /dev/null; then
            (cd target/dist && zip -qr "$name.zip" "$name")
        else
            (cd target/dist && powershell.exe -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$name.zip'")
        fi
        echo "target/dist/$name.zip"
    else
        tar -C target/dist -czf "target/dist/$name.tar.gz" "$name"
        echo "target/dist/$name.tar.gz"
    fi
}

case "${1:-}" in
    bootstrap) bootstrap ;;
    package) package ;;
    *) echo "usage: sh scripts/dist.sh bootstrap|package" >&2; exit 2 ;;
esac
