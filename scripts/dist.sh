#!/bin/sh
# Builds brine from the bootstrap seed and packages it for this machine. CI and
# releases run it from the repository root; it works the same by hand.
#
#   sh scripts/dist.sh bootstrap   seed -> target/boot/brine0 -> ./brine (the compiler under test)
#   sh scripts/dist.sh package     ./brine -> target/dist/brine-<os>-<arch>.tar.gz (.zip on Windows)
#
# Needs a C compiler: $CC, or cc (gcc on Windows, from MSYS2 or MinGW).
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
cc="${CC:-cc}"
cflags="-std=c11 -O2 -fno-strict-aliasing -w -Iruntime"
case "$os" in
    linux) libs="-lm -lpthread -ldl" ;;
    # the oldest macOS that runs on Apple silicon
    macos) libs="-lm -lpthread"; cflags="$cflags -mmacosx-version-min=11.0" ;;
    # static, so the binary doesn't need MinGW's DLLs (libwinpthread)
    windows) libs="-static -lws2_32 -lshell32"; exe=".exe"; cc="${CC:-gcc}" ;;
esac

bootstrap() {
    mkdir -p target/boot
    echo "building the seed with $cc"
    # shellcheck disable=SC2086
    "$cc" $cflags bootstrap/brinec.c runtime/brine_rt.c -o "target/boot/brine0$exe" $libs
    # brine0 carries the seed's copy of the runtime, which may predate fixes in
    # runtime/; build with the C compiler that just built it, which handles it.
    BRINE_CC="$cc" "target/boot/brine0$exe" build compiler/main.br -o "brine$exe"
    "./brine$exe" --version
}

package() {
    [ -x "./brine$exe" ] || { echo "dist.sh: run 'sh scripts/dist.sh bootstrap' first" >&2; exit 1; }
    out="target/dist/$name"
    rm -rf target/dist
    mkdir -p "$out"
    # A release build through C, with flags that suit a binary for other machines.
    "./brine$exe" emit-c --release compiler/main.br -o target/dist/brine.c
    # shellcheck disable=SC2086
    "$cc" $cflags target/dist/brine.c runtime/brine_rt.c -o "$out/brine$exe" $libs
    cp LICENSE README.md "$out/"

    # The packaged compiler must work on its own, outside the repository.
    smoke="$(mktemp -d)"
    cp "$out/brine$exe" "$smoke/"
    printf 'fn main():\n    print("hello from", par sum(i for i in 0..10))\n' > "$smoke/hello.br"
    (cd "$smoke" && "./brine$exe" --version && "./brine$exe" run hello.br -q) > "$smoke/log" 2>&1 || {
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
