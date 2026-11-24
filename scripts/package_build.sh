#!/bin/sh
# Packs a finished build tree so another Mac can run it with scripts/downlevel_check.sh.
# tar keeps the executable bits and the bundle's ad hoc signature, which artifact uploads drop.
# Object files and CMake's internal state are left out; CTest needs only the test files and binaries.
# Usage: scripts/package_build.sh [build-dir] [archive.tar.gz]
set -eu

build=${1:-build}
archive=${2:-build-release.tar.gz}

tar -czf "$archive" --exclude CMakeFiles --exclude Testing --exclude '*.o' "$build"
echo "Packed $build into $archive ($(du -h "$archive" | cut -f1))"
