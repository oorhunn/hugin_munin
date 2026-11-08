# Progress

What has been accomplished, newest first.

## 2026-11-08

- Replaced the root `.clang-format` with a new style and reformatted all C++ and Objective-C++ to
  it. The style uses a 2-space indent, 80 columns, no short blocks, functions, or loops on one line,
  and braces on their own line after functions and enums. The requested file also set
  `Language: Cpp`, which was left out because clang-format then refuses `pasteboard.mm` and the CI
  Format job fails. The change is formatting only. Local clang-format and clang-tidy 23.1.4, a
  Debug build, all six tests, and the launch smoke check pass. Open branches will need to be
  reformatted or will conflict when they merge this.
- Merged main’s clang-tidy CI setup into the clipboard branch, retaining both documentation
  changes. Made the formatter language-neutral so Objective-C++ remains supported, and
  fixed clipboard findings under the new lint checks (unsigned byte masks, size arithmetic,
  explicit/nodiscard declarations, scoped locks, and a const value preventing a move).
  Local clang-tidy 23.1.4, formatting, build, all six tests, and launch smoke check pass.

- Changed the keyboard preview to hold-to-browse: release all shortcut modifiers to restore the
  selection, dismiss, and send Cmd+V to the original app. Escape and focus loss cancel.
  Automatic paste requires Accessibility permission, requested through Settings; without it,
  release still copies and closes. Delivery is skipped if the original app is no longer active.
  The preview background is now 70% opaque and the file shelf is hidden while previewing.
- Release-to-paste local validation: Swift build, all six tests, and the 10-second launch
  smoke check pass. Interactive permission, focus, and paste delivery remain unverified
  on a physical desktop session.

- Changed the default clipboard shortcut to Cmd+Shift+V and migrated the former default while
  retaining custom shortcuts. The shortcut opens a keyboard-focused preview inside the notch:
  left/right browse a selected text/image, Return restores it, and Escape closes. Keyboard mode
  stays open independently of pointer hover and dismisses on focus loss. Browsing preserves
  selection by entry id when new copies arrive, falling back to the newest if an entry is evicted.
- Keyboard preview validation: local Swift build, all six tests, and the 10-second launch smoke
  test pass. Physical keyboard interaction and focus return remain unverified; a manual
  checklist was added to `development.md`.

- Merged main’s file shelf into the clipboard branch, retaining both features in the notch
  and extending the panel to fit both. The combined build, all six tests, formatting, and
  the 10-second app launch smoke test pass. Interactive shelf and clipboard layout remains
  unverified on a physical notch display.

- Fixed PR CI formatting violations using the same clang-format 23.1.4 version as CI.
- Replaced the launch-at-login binding's method reference with an explicit closure to avoid
  the Swift 6.3.3 compiler's crashing isolated Boolean callback reabstraction thunk.
  Local build, all five tests, formatting check, and the 10-second app launch smoke test pass
  with Swift 6.4. Verification on the hosted Swift 6.3.3 toolchain remains pending.
## 2026-11-06

- Merged `main` into the GPU and CI branch. The first CI run of the branch failed in both builds
  with the Swift 6.3.3 compiler crash described below, because the branch predated the fix to the
  login toggle in `ced4a04`. The merge brings that fix in, and it needed no other Swift change.
  The merged shelf code had one clang-tidy warning, fixed with `[[nodiscard]]`.
- Added `clang-tidy` to the project and to CI.
  - `.clang-tidy` covers bug-finding and modernization checks, `tests/.clang-tidy` turns off three
    checks that do not fit test code, and style-only checks stay off.
  - A new Lint job runs clang-tidy 23.1.4 and fails on any warning.
  - `.clang-format` now names the language and the latest C++ standard. No file changed format.
  - `third_party/.clang-format` turns formatting off for vendored code.
  - Fixed the 9 warnings in the committed code, with no change in behavior: an `explicit`
    constructor, `std::scoped_lock`, a `stop_token` passed by reference, an explicit `void *` cast,
    and clearer declarations.
- Investigated a Swift 6.3.3 CI compiler crash generating an isolated Boolean callback wrapper.
  Shelf callback annotations and then direct notch state access did not resolve it. Located a
  remaining bound-method conversion in the Launch at login binding and replaced it with an
  explicit closure. Local Swift 6.4 build, tests, and smoke test pass. All five CI checks passed
  on commit `ced4a04`, including Debug and Release builds on Swift 6.3.3.

- Added a temporary file/folder shelf to the expanded notch, with drops onto the collapsed trigger
  opening the panel. Shelf entries show file icons, support dragging to external destinations,
  and have a remove button that leaves originals intact.
- Added C++ shelf membership and a C bridge, including normalization and duplicate rejection.
- Built the app, passed all five test programs, and passed the 10-second launch smoke test.
- Real Finder drag/drop, copy/move, cancellation, scrolling, and physical notch interactions
  remain unverified; the manual checklist is in `development.md`.

## 2026-11-05

- Added the copy/paste buffer: a clipboard history of text and images, restored on click.
  - The core keeps a bounded history (`core/src/clipboard.cpp`): 50 entries by default, 1 to 500
    in Settings, and at most 64 MiB of payload in total, evicting the oldest first. Content equal
    to an existing entry reuses it and moves it to the front, so each distinct payload appears
    once, most recent first. Ids are never reused.
  - The pasteboard sits behind a second platform boundary (`core/include/hm/pasteboard.hpp`),
    implemented for macOS in `core/src/platform/macos/pasteboard.mm` with `NSPasteboard`: text is
    preferred when both are present, images are kept as TIFF, and PNG is converted. The watcher
    polls the change count every 0.5 s, because there is no event API for other processes' copies
    without privileges. Its own writes are marked seen, so a restore is not recorded as a copy.
  - The bridge grew the `hm_clipboard_*` C API: a malloc'd deep-copy snapshot with a 255-byte
    UTF-8 text preview per entry, capacity changes while running, restore, and clear.
  - The notch panel now shows the history below the metrics: click an entry to put it back on the
    clipboard, then paste with Cmd+V. Rows show a thumbnail, the preview, the byte size, and a
    relative time; a Clear button empties the history. The expanded panel grew from 112 to 320
    points to fit the list.
  - A global shortcut opens the history by expanding the notch: Cmd+Shift+C by default, editable
    in Settings through a key recorder, saved in UserDefaults with the capacity. It is registered
    with Carbon's `RegisterEventHotKey`, which needs no Accessibility permission.
  - Restoring writes the entry back to the pasteboard and moves it to the front. Pasting into the
    frontmost app automatically would need Accessibility permission, so the user pastes with
    Cmd+V; this is the documented semantic.
- Fixed during testing: a poll can land inside another process's pasteboard write, between the
  clear and the new contents, when the change count has already moved but the data is not there.
  The first version marked such a count as seen and lost the copy forever; `bridge_test` failed
  about two runs in three because of it. The tracker now retries an empty or failed read once on
  the next poll before treating the count as settled.
- Tests:
  - `clipboard_test` drives the core with a fake pasteboard: history and dedup rules, capacity
    clamps and eviction, the byte budget, image and kind handling, the poll's retry for mid-write
    and failed reads, own-write suppression, and the watcher thread's notifications, restore,
    live capacity change, and clear.
  - `bridge_test` gained a live pasteboard round trip: start, two text copies, restore, image
    bytes across the bridge, clear, stop. It temporarily replaces the clipboard's contents, which
    is unavoidable behind the C interface.
- Verified on macOS 27.0.1 (Apple clang 21, Swift 6):
  - All five test programs pass in Debug, and under ASan with UBSan. `clipboard_test` and
    `bridge_test` pass under TSan; TSan first flagged a race in the test's own fake pasteboard,
    fixed by giving the fake a mutex, like the system gives the real one.
    `bridge_test` was run five more times to check the mid-write fix holds.
  - `clang-format` is clean, and the check now covers `*.mm` in CI and the docs, because the
    pasteboard backend is Objective-C++.
  - `scripts/smoke_test.sh` passes: the app stays running for 10 s. Launched by hand, it also
    stayed alive through two real copies made with `pbcopy`.
- Not verified by hand: the notch list itself and the Cmd+Shift+C shortcut on a real keyboard, as
  that needs an interactive session on a Mac with a notch. A shortcut that another app already
  registered is silently unavailable; the register call's result is not surfaced in the UI. The
  list also has no keyboard navigation, and its relative timestamps refresh only when the
  history changes.
- Added GPU usage and put CPU, memory, disk, and GPU side by side in the notch and its panel.
  - Ported GPU reading from `resource_visualizer`, keeping only utilization and core count. With
    several GPUs, the notch shows the busiest one.
  - Replaced the left and right pickers in Settings with a checkbox per metric. The wings and their
    hover area resize to fit. The panel widened from 480 to 640 points and always shows all four.
- Verified on a MacBook Pro with an M2 Pro:
  - All four test programs pass.
  - GPU core count matched the system (19). Under a sustained Metal load, GPU read 99%, the same as
    `ioreg`, then fell back afterwards. A read takes about 25 µs.
  - The app showed all four metrics, with two per side. Three metrics split as two and one. Two
    metrics narrowed the notch live.
  - With two metrics, hovering inside the narrower wing expanded the panel 5 of 5 times, and
    hovering at the old outer edge did not, 5 of 5 times.
- Known issues:
  - The driver seems to report GPU activity since the last read, so a second read within
    milliseconds returns 0. With `ioreg`, whichever ran second read 0. Another tool polling the GPU
    at the same moment could make this app briefly show 0%.
  - One hover inside the wing failed to expand the panel during testing. It did not happen again in
    the 7 later attempts on the same code, and the cause is unknown.
  - The wider notch covers more of the menu bar, so it can hide menu bar items. This was not
    checked with a full menu bar.
- Published the project to https://github.com/oorhunn/hugin_munin as a public repository.
- Added the settings menu UI and the README icon.
  - A Settings button in the expanded notch panel opens a small window with the update interval,
    the metric on each side of the notch, and launch at login.
  - `start_sampling` became a `Sampler` class so that the interval can change while it runs.
  - The second sample now arrives within a second whatever the interval, because CPU showed as
    unknown for a full interval after launch. Found while testing a 5 second interval.
  - Added the icon at `doc/media/icon.png`, resized from 1254 to 512 pixels. It reads well on light
    and dark backgrounds.
- Verified on the real app:
  - All four test programs pass, and `sampler_test` and `bridge_test` pass under TSan.
  - Saved settings load at launch, and changes are saved.
  - The interval changes live: over 3 seconds the notch showed 1 distinct image at 10 s and 7 at
    0.5 s.
  - Changing a side of the notch updates it live.
  - Launch at login registered with macOS and unregistered again, with no error.
- Not tested: an actual login. A login item is tied to the app's location, so one enabled from
  `build/` stops working if that folder is deleted.
- Added the first runnable app, `build/HuginMunin.app`, built entirely through CMake.
  - `start_sampling` reads CPU and memory every second and disk every 60 seconds on a background
    thread. A failed memory or disk read keeps the last value, and a failed CPU read shows as
    unknown.
  - Added the C bridge (`hm_bridge.h`) that the Swift UI uses.
  - Ported the notch from `resource_visualizer`: window handling and hover are unchanged. The
    expanded panel shows CPU, memory, disk, and a Quit button. It has no GPU, process list, chip
    view, or dashboard link. The old app's menu bar pill for screens with no notch came across
    with this code, but that behaviour is still postponed and was not tested.
- Verified on a MacBook Pro with an M2 Pro, with an external display as the main screen:
  - All four test programs pass in Debug and Release, and under ASan and UBSan. `sampler_test` and
    `bridge_test` also pass under TSan.
  - Ran the real app: it picked the built-in display, showed live values, expanded on hover,
    collapsed when the pointer left, and exited through Quit with no crash report.
  - The panel's disk figures matched Foundation (70.6 GB free, 85% used).
  - Used about 51 MB of memory and 0.53 s of CPU time over its first 54 s.
- Problems fixed along the way:
  - The app crashed on launch because the C callback inherited `@MainActor` isolation and trapped
    on the sampling thread. All tests passed anyway, because they do not cover Swift. The callback
    is now a nonisolated top-level function.
  - `swiftc` leaves dependency files inside the bundle, which breaks signing, and they have
    different names in Debug and Release. The build now deletes both.
  - Incremental Swift builds failed on relink with `cannotResolveTempPath`, so the app now always
    builds in whole-module mode. This raised the minimum CMake version to 3.29.
- Not done: an app icon and Ubuntu support. The bundle is signed ad hoc only, so it is not ready to
  distribute.
- Added the C++23 core (`hm_core`), ported from `resource_visualizer`, with a CMake build and tests.
  - CPU per-core ticks, usage, and average, and system memory, ported from its C++ backend.
  - Disk usage for the startup volume, rewritten in C++ using CoreFoundation because the original
    was in Swift. Purgeable space counts as free, as before.
  - Collectors now return `std::expected` instead of throwing.
  - Not ported: processes, GPU, and P/E core topology.
- Verified on macOS 27.0.1 (Apple clang 21):
  - Both test programs pass, including under ASan and UBSan.
  - Sampled at the same time, CPU average was within about 2 points of `top` and wired memory
    within 10 pages of `vm_stat`.
  - Memory total, core count, swap, and compressed memory matched system tools exactly. Disk total
    matched Foundation exactly, and free space was within 0.3% across separate readings.
  - `collect_disk` takes about 8 to 10 ms. Memory and CPU together take about 23 µs.
- Set up the project rules and documentation: `README.md`, `CLAUDE.md`, `AGENTS.md` (points to
  `CLAUDE.md`), and `doc/`.
