# Development

## Requirements

- macOS 14 or later to run the app
- CMake 3.29 or later, Ninja, a C++23 compiler, and Swift 6
- `clang-format` and `clang-tidy` to check C++, from `brew install clang-format llvm`

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
| `clipboard_test` | No | The clipboard history and watcher with a fake pasteboard |
| `bridge_test` | Yes | The C interface, through the real backend and pasteboard |
| `file_shelf_test` | No | Shelf bridge, path normalization, duplicate rejection, removal, and bounds |

Tests use the small `require` helper in `tests/check.hpp` rather than a framework.

`bridge_test` temporarily replaces the clipboard's contents to exercise the copy/paste buffer,
because there is no fake pasteboard behind the C interface. Run it when you can spare the
clipboard for a few seconds.

The tests do not cover Swift. After changing `ui/macos/` or the bridge, confirm the app stays
running, because a Swift concurrency error can pass every test and still crash on launch:

```sh
scripts/smoke_test.sh build/HuginMunin.app
```

This confirms that the app launches. It does not confirm that the UI behaves correctly. If more
than one person or agent works in the same checkout, note that `pkill -x HuginMunin` also ends
another build's smoke test, which then reports a failure with status 143.

## Sanitizers

The presets instrument both C++ and Objective-C++, including the macOS pasteboard backend.
Build only the tests, because the Swift app does not link against sanitized objects.

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
git ls-files '*.cpp' '*.hpp' '*.h' '*.mm' | xargs clang-format --dry-run -Werror  # check
git ls-files '*.cpp' '*.hpp' '*.h' '*.mm' | xargs clang-format -i                 # fix
```

Swift formatting is not checked yet. `third_party/.clang-format` turns formatting off for vendored
code, so these commands leave it alone.

## Linting

`.clang-tidy` turns on the bug-finding and modernization check groups, and turns off checks that
only enforce a style this project does not use. Any warning fails the check. `tests/.clang-tidy`
adds to this for test code. CI uses clang-tidy 23.1.4.

Homebrew does not put `clang-tidy` on `PATH`, and the SDK path has to be passed in. Configure the
build first, because the command reads its compile database.

```sh
git ls-files '*.cpp' '*.mm' | xargs -P 6 -n 1 "$(brew --prefix llvm)/bin/clang-tidy" --quiet \
    -p build --extra-arg=-isysroot"$(xcrun --show-sdk-path)"
```

Results from vendored and SDK headers are filtered out. If a warning is wrong for one line, use
`// NOLINT(check-name)` with a reason, and if it is wrong for the project, turn the check off in
`.clang-tidy`.

## Deployment target

The app targets macOS 14. `CMakeLists.txt` passes this to `swiftc` as an explicit target, because
`swiftc` otherwise builds for the host's macOS and ignores `CMAKE_OSX_DEPLOYMENT_TARGET`. To check
a build:

```sh
vtool -show-build build/HuginMunin.app/Contents/MacOS/HuginMunin
```

## Manual shelf checks

On a Mac, hover the notch with an empty shelf and confirm the panel is short with no shelf.
Drag a disposable document and folder from Finder onto the collapsed notch, then
check both entries in the expanded shelf at full height. Drag each to a Finder destination and verify the
destination and original according to Finder's chosen copy/move operation. Repeat with Option
held for a copy. Cancel a drag with Escape and confirm its entry remains. Use × and confirm
the original still exists. Remove the last entry and confirm the panel shrinks again. Try duplicate drops, enough entries to scroll, and an item renamed
outside the app. Quit and reopen to confirm the session shelf clears.

### Clipboard keyboard preview checklist

These checks need a macOS desktop session, and they have not been run. Enable Accessibility with
Settings → Automatic paste. A rebuilt ad hoc signed app may need the permission granted again.

Copy several text entries and an image. In an editor:

- Hold Cmd+Shift+V, keep the modifiers held, and tap V. Each tap should move one entry older, and
  the counter should stop at the oldest entry.
- Release all modifiers. The selection should paste exactly once, the preview should close, and
  typing should go to the editor straight away.
- Open the preview again. It should start on the entry just pasted, and the history order should
  be unchanged. Copy something new and confirm the next preview starts at 1.
- Try the arrows, J and K, Home, End, Page Up, Page Down, and the number keys, with 0 as the tenth
  entry. Delete should forget the entry. Return should paste. Keypad Enter should copy without
  pasting. Escape should change nothing.
- Tap the shortcut quickly, release the modifiers in both orders, open with an empty history, and
  switch apps while the preview is open, which should cancel without pasting.
- The preview should appear in the middle of the screen, including over a full-screen app. Scroll
  over it to move through the history, and double click it to paste.
- Hover over the notch and confirm it shows metrics, Settings, Quit, and the file shelf when it holds items, with no
  clipboard content.
- Quit and reopen the app. The history should come back in order. Clear History in Settings, and
  turn off Keep history after quitting, should each delete
  `~/Library/Application Support/HuginMunin/clipboard-history.bin`.
- Copy a password from a password manager and confirm it is not recorded.
- Turn off Accessibility and confirm that choosing an entry copies it and shows the hint.
- Record a custom shortcut and repeat the first check with it.
