# Development

## Requirements

- macOS 14 or later to run the app
- CMake 3.29 or later, Ninja, a C++23 compiler, and Swift 6
- `clang-format` to check formatting

The Xcode command line tools are enough. The full Xcode app is not needed.

## Build, test, and run

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
open build/HuginMunin.app
```

To quit the app, hover over the notch and click **Quit**.

## Tests

| Test | Needs a live system | Covers |
| --- | --- | --- |
| `metrics_test` | No | Metric calculations |
| `sampler_test` | No | Sampling logic with fake sources |
| `collectors_test` | Yes | The platform backend |
| `bridge_test` | Yes | The C interface, through the real backend |
| `file_shelf_test` | No | Shelf bridge, path normalization, duplicate rejection, removal, and bounds |

Tests use the small `require` helper in `tests/check.hpp` rather than a framework.

The tests do not cover Swift. After changing `ui/macos/` or the bridge, confirm the app stays
running, because a Swift concurrency error can pass every test and still crash on launch:

```sh
scripts/smoke_test.sh build/HuginMunin.app
```

This confirms that the app launches. It does not confirm that the UI behaves correctly. If more
than one person or agent works in the same checkout, note that `pkill -x HuginMunin` also ends
another build's smoke test, which then reports a failure with status 143.

## Sanitizers

The presets build only the tests, because the Swift app does not link against sanitized objects.

```sh
cmake --preset asan-ubsan
cmake --build build/asan-ubsan --target hm_tests
ctest --test-dir build/asan-ubsan --output-on-failure

cmake --preset tsan
cmake --build build/tsan --target hm_tests
ctest --test-dir build/tsan --output-on-failure
```

## Formatting

C++ follows `.clang-format`. CI uses clang-format 23.1.4, and other versions may disagree on some
lines.

```sh
git ls-files '*.cpp' '*.hpp' '*.h' | xargs clang-format --dry-run -Werror  # check
git ls-files '*.cpp' '*.hpp' '*.h' | xargs clang-format -i                 # fix
```

Swift formatting is not checked yet.

## Deployment target

The app targets macOS 14. `CMakeLists.txt` passes this to `swiftc` as an explicit target, because
`swiftc` otherwise builds for the host's macOS and ignores `CMAKE_OSX_DEPLOYMENT_TARGET`. To check
a build:

```sh
vtool -show-build build/HuginMunin.app/Contents/MacOS/HuginMunin
```

## Manual shelf checks

On a Mac, drag a disposable document and folder from Finder onto the collapsed notch, then
check both entries in the expanded shelf. Drag each to a Finder destination and verify the
destination and original according to Finder's chosen copy/move operation. Repeat with Option
held for a copy. Cancel a drag with Escape and confirm its entry remains. Use × and confirm
the original still exists. Try duplicate drops, enough entries to scroll, and an item renamed
outside the app. Quit and reopen to confirm the session shelf clears.
