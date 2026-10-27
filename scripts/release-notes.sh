#!/bin/sh
# Prints the GitHub release notes for version X.Y.Z: its CHANGELOG.md section,
# how to install it, and a link to every change since the previous release.
# Fails when VERSION in compiler/driver.br or the changelog doesn't match, so a
# release can't go out with the wrong version.
#
#   sh scripts/release-notes.sh 0.4.0 > notes.md
set -eu

ver="${1:?usage: sh scripts/release-notes.sh X.Y.Z}"
ver="${ver#v}"
repo="https://github.com/brinelang/brine"

have=$(sed -n 's/^const VERSION = "\(.*\)"$/\1/p' compiler/driver.br)
if [ "$have" != "$ver" ]; then
    echo "release-notes.sh: compiler/driver.br has VERSION $have, not $ver" >&2
    exit 1
fi

# The lines after "## [X.Y.Z]" up to the next "## [" (or the link list).
section=$(awk -v v="$ver" '
    index($0, "## [" v "]") == 1 { on = 1; next }
    on && (/^## \[/ || /^\[[^]]+\]: /) { exit }
    on { print }
' CHANGELOG.md)
if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "release-notes.sh: CHANGELOG.md has no section for $ver" >&2
    exit 1
fi

printf '%s\n' "$section" | sed -e '/./,$!d'
cat <<EOF

## Install

Linux and macOS:
\`\`\`sh
curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
\`\`\`

Windows (PowerShell):
\`\`\`powershell
irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
\`\`\`

brine needs a C compiler (\`gcc\` or \`clang\`; MinGW on Windows): run \`brine doctor\` to check.
To install this version rather than the latest, set \`BRINE_VERSION=$ver\` first. The
archives below are checked against \`SHA256SUMS\` by the installers, and each has a
[build provenance attestation]($repo/attestations) (\`gh attestation verify <file> -R brinelang/brine\`).
EOF

prev=$(git describe --tags --abbrev=0 --match 'v[0-9]*' "v$ver^" 2>/dev/null || true)
if [ -n "$prev" ]; then
    printf '\n**Full changelog**: %s/compare/%s...v%s\n' "$repo" "$prev" "$ver"
fi
