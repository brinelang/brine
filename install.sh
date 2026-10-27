#!/bin/sh
# Installs brine on Linux or macOS:
#
#   curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
#
# Settings (environment variables):
#   BRINE_VERSION=0.4.0      a version (default: the latest release)
#   BRINE_INSTALL=~/.brine   where to install (the binary goes in $BRINE_INSTALL/bin)
#   BRINE_NO_MODIFY_PATH=1   don't add brine to PATH in your shell's startup file
#   BRINE_DOWNLOAD_URL=...   where the release files are (default: GitHub releases; for mirrors)
#
# To uninstall, delete $BRINE_INSTALL (~/.brine) and its line in your shell's
# startup file.
set -eu

# Nothing runs until the last line calls main, so a download cut short does nothing.
main() {
    repo="brinelang/brine"
    install_dir="${BRINE_INSTALL:-$HOME/.brine}"
    bin_dir="$install_dir/bin"

    case "$(uname -s)" in
        Linux) os=linux ;;
        Darwin) os=macos ;;
        MINGW* | MSYS* | CYGWIN*)
            die "on Windows, install from PowerShell: irm https://raw.githubusercontent.com/$repo/main/install.ps1 | iex" ;;
        *) die "brine has no build for $(uname -s); build it from source: https://github.com/$repo" ;;
    esac
    case "$(uname -m)" in
        x86_64 | amd64) arch=x64 ;;
        aarch64 | arm64) arch=arm64 ;;
        *) die "brine has no build for $(uname -m) CPUs; build it from source: https://github.com/$repo" ;;
    esac
    # An x64 shell on Apple silicon (Rosetta) still gets the native build.
    if [ "$os" = macos ] && [ "$arch" = x64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || true)" = 1 ]; then
        arch=arm64
    fi

    name="brine-$os-$arch"
    version="${BRINE_VERSION:-}"
    version="${version#v}"
    if [ -n "${BRINE_DOWNLOAD_URL:-}" ]; then
        base="${BRINE_DOWNLOAD_URL%/}"
    elif [ -n "$version" ]; then
        base="https://github.com/$repo/releases/download/v$version"
    else
        base="https://github.com/$repo/releases/latest/download"
    fi

    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT INT TERM

    say "downloading $name ${version:-(latest)}"
    fetch "$base/$name.tar.gz" "$tmp/$name.tar.gz" ||
        die "download failed: $base/$name.tar.gz${version:+ (is $version a released version?)}"
    fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/SHA256SUMS"

    want="$(awk -v f="$name.tar.gz" '{ n = $2; sub(/^\*/, "", n) } n == f { print $1; exit }' "$tmp/SHA256SUMS")"
    [ -n "$want" ] || die "SHA256SUMS has no entry for $name.tar.gz"
    got="$(sha256 "$tmp/$name.tar.gz")"
    [ "$got" = "$want" ] || die "checksum mismatch for $name.tar.gz (expected $want, got $got)"

    tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
    mkdir -p "$bin_dir"
    # Replace through a rename, so a running brine isn't overwritten in place.
    cp "$tmp/$name/brine" "$bin_dir/brine.new"
    chmod +x "$bin_dir/brine.new"
    mv -f "$bin_dir/brine.new" "$bin_dir/brine"
    say "installed $("$bin_dir/brine" --version) to $bin_dir/brine"

    add_to_path

    if ! command -v cc > /dev/null 2>&1 && ! command -v gcc > /dev/null 2>&1 && ! command -v clang > /dev/null 2>&1; then
        if [ "$os" = macos ]; then
            hint="xcode-select --install"
        else
            hint="your package manager (for example: sudo apt install gcc, sudo dnf install gcc)"
        fi
        warn "brine needs a C compiler to build programs, and none was found; install one with $hint"
    fi
    say "run 'brine doctor' to check your setup, and 'brine new hello' to start a project"
}

add_to_path() {
    case ":$PATH:" in
        *":$bin_dir:"*) return ;;
    esac
    if [ -n "${BRINE_NO_MODIFY_PATH:-}" ]; then
        say "add $bin_dir to your PATH to use brine"
        return
    fi
    line="export PATH=\"$bin_dir:\$PATH\""
    case "$(basename "${SHELL:-sh}")" in
        zsh) rc="${ZDOTDIR:-$HOME}/.zshrc" ;;
        bash)
            if [ "$os" = macos ]; then rc="$HOME/.bash_profile"; else rc="$HOME/.bashrc"; fi ;;
        fish)
            rc="$HOME/.config/fish/conf.d/brine.fish"
            line="fish_add_path \"$bin_dir\""
            mkdir -p "$(dirname "$rc")" ;;
        *) rc="$HOME/.profile" ;;
    esac
    if [ -f "$rc" ] && grep -qF "$bin_dir" "$rc"; then
        :
    else
        printf '\n# brine\n%s\n' "$line" >> "$rc"
        say "added $bin_dir to PATH in $rc"
    fi
    say "restart your shell, or run: $line"
}

fetch() {
    if command -v curl > /dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2" "$1"
    elif command -v wget > /dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        die "this needs curl or wget"
    fi
}

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{ print $1 }'
    else
        die "this needs sha256sum or shasum to check the download"
    fi
}

say() { printf 'brine: %s\n' "$1"; }
warn() { printf 'brine: warning: %s\n' "$1" >&2; }
die() {
    printf 'brine: error: %s\n' "$1" >&2
    exit 1
}

main "$@"
