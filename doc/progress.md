# Progress

What has been accomplished, newest first.

## 2026-11-06

- CI's Swift 6.3.3 compiler crashed generating a Boolean callback wrapper for shelf drag state.
  Explicit main-actor callback annotations did not resolve the CI crash. Replaced that callback
  chain with direct main-actor notch state access from the AppKit drag source.
  Local Swift 6.4 build, tests, and smoke test pass; CI confirmation is pending.

- Added a temporary file/folder shelf to the expanded notch, with drops onto the collapsed trigger
  opening the panel. Shelf entries show file icons, support dragging to external destinations,
  and have a remove button that leaves originals intact.
- Added C++ shelf membership and a C bridge, including normalization and duplicate rejection.
- Built the app, passed all five test programs, and passed the 10-second launch smoke test.
- Real Finder drag/drop, copy/move, cancellation, scrolling, and physical notch interactions
  remain unverified; the manual checklist is in `development.md`.

## 2026-11-05

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
