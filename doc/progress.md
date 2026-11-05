# Progress

What has been accomplished, newest first.

## 2026-11-05

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
