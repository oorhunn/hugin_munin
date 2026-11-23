#!/bin/sh
# Runs an already-built Release tree on the current Mac without rebuilding it. CI uses this to run
# the macOS 26 build on the oldest hosted runner, and the manual macOS 14 release check uses it too.
# The build tree must sit at the absolute path it was built in, because CTest records full paths.
# Usage: scripts/downlevel_check.sh [build-dir] [launch-seconds]
set -eu

build=${1:-build}
seconds=${2:-10}
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

if ! grep -qF "$(cd "$build" && pwd)" "$build/CTestTestfile.cmake"; then
    echo "$build was built at another path; unpack it where it was built" >&2
    exit 1
fi

echo "== Host"
sw_vers
uname -m

echo "== Minimum OS metadata"
python3 "$here/check_macos_compatibility.py" "$build/HuginMunin.app"

echo "== Tests built elsewhere, run here"
# The registered mcp_integration command names the build machine's Python, which may not exist
# here, so that one test runs below with this machine's python3 instead.
ctest --test-dir "$build" --output-on-failure --exclude-regex '^mcp_integration$'
python3 "$root/tests/mcp_integration.py" "$build/hm-mcp"
echo "mcp_integration passed"

echo "== Launch"
"$here/smoke_test.sh" "$build/HuginMunin.app" "$seconds"
