#!/bin/sh
# Formats and lints the tracked Swift sources with pinned tool versions, so a developer and CI
# run the same checks. The tools are downloaded once into .cache/swift-tools/, and each download
# is verified against a SHA-256 checksum before use.
# Usage: scripts/swift_tools.sh check|format|install
set -eu

SWIFTFORMAT_VERSION=0.64.7
SWIFTFORMAT_SHA256=e82e224325cbd3403fc7b68caf6c917926dc106de3af875fca4b87e3c4634038
SWIFTLINT_VERSION=0.65.1
SWIFTLINT_SHA256=c1e429b0599cf1b516f369a2d9ec04eaf0e436f3c12b637df8851fa52ff694d0

root=$(cd "$(dirname "$0")/.." && pwd)
cache=${HM_SWIFT_TOOLS_DIR:-$root/.cache/swift-tools}
swiftformat="$cache/swiftformat-$SWIFTFORMAT_VERSION/swiftformat"
swiftlint="$cache/swiftlint-$SWIFTLINT_VERSION/swiftlint"

# fetch <name> <url> <sha256> <binary>: downloads and unpacks a zip unless the binary exists.
fetch() {
    name=$1 url=$2 sha=$3 binary=$4
    [ -x "$binary" ] && return 0
    dir=$(dirname "$binary")
    work=$(mktemp -d)
    echo "Downloading $name" >&2
    curl --fail --silent --show-error --location --output "$work/tool.zip" "$url"
    actual=$(shasum -a 256 "$work/tool.zip" | cut -d' ' -f1)
    if [ "$actual" != "$sha" ]; then
        echo "Checksum mismatch for $name: expected $sha, got $actual" >&2
        rm -rf "$work"
        exit 1
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    unzip -q "$work/tool.zip" -d "$dir"
    # The SwiftLint archive names its executable swiftlint; SwiftFormat ships a capitalised one.
    [ -x "$binary" ] || mv "$dir/$(ls "$dir" | grep -i "^$(basename "$binary")\$" | head -n 1)" "$binary"
    chmod +x "$binary"
    rm -rf "$work"
}

install_tools() {
    fetch swiftformat \
        "https://github.com/nicklockwood/SwiftFormat/releases/download/$SWIFTFORMAT_VERSION/swiftformat.zip" \
        "$SWIFTFORMAT_SHA256" "$swiftformat"
    fetch swiftlint \
        "https://github.com/realm/SwiftLint/releases/download/$SWIFTLINT_VERSION/portable_swiftlint.zip" \
        "$SWIFTLINT_SHA256" "$swiftlint"
}

# The portable SwiftLint binary loads sourcekitd from the toolchain, and it only finds the Command
# Line Tools layout when told where the toolchain is.
if [ -z "${TOOLCHAIN_DIR:-}" ]; then
    developer=$(xcode-select -p)
    xctoolchain="$developer/Toolchains/XcodeDefault.xctoolchain"
    if [ -d "$xctoolchain" ]; then TOOLCHAIN_DIR=$xctoolchain; else TOOLCHAIN_DIR=$developer; fi
    export TOOLCHAIN_DIR
fi

cd "$root"
mode=${1:-check}
case "$mode" in
install)
    install_tools
    ;;
check | format)
    install_tools
    files=$(git ls-files 'ui/macos/*.swift')
    status=0
    if [ "$mode" = format ]; then
        # shellcheck disable=SC2086
        "$swiftformat" --config .swiftformat --quiet $files
    else
        # shellcheck disable=SC2086
        "$swiftformat" --config .swiftformat --lint $files || status=1
    fi
    # shellcheck disable=SC2086
    "$swiftlint" lint --config .swiftlint.yml --strict --quiet $files || status=1
    exit $status
    ;;
*)
    echo "Usage: $0 check|format|install" >&2
    exit 2
    ;;
esac
