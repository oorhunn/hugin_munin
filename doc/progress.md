# Progress

What has been accomplished, newest first.

## 2026-11-06

- Moved the MCP coordination work onto its own branch, on top of the GPU, CI, and shelf work.
  - Both features add to the expanded notch panel, so it is now 446 points tall and shows the jobs
    list above the shelf. This combined layout was not checked visually.
  - The pause control now passes an explicit closure, because the bound method it used is the
    pattern that crashes the Swift 6.3.3 compiler on CI.
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
  - Resolved the 45 warnings left in the coordinator and utilities code, and in the bridge.
    `coordinator_ipc.cpp` now uses `getpwuid_r` and `memcpy`, and stops if `socket` fails. `hm-mcp`
    keeps its flush after each reply, because `'\n'` alone would leave clients waiting. `setenv` in
    `hm-run` and three deliberate empty `catch` blocks are annotated `NOLINT` with their reasons.
- Verified locally with clang-tidy 23.1.4 and no warnings, on the committed code and on the working
  tree. All tests pass in Debug, Release, and under both sanitizer presets, 4 tests on the commit and
  6 with the coordinator, and the app stayed running for 8 seconds in Debug and Release. A planted
  warning fails the check.

- Added bundled C++23 `hm-service`, `hm-mcp`, and `hm-run` utilities. A private per-user service
  now owns sampling and advisory CPU/RAM/disk/GPU reservations shared by the app and MCP clients.
- Added registration, FIFO job admission, application-supplied and revised estimates, heartbeats,
  conservative orphan holds, ownership checks, bounded history, memory pressure, and freshness.
- Expanded the existing notch hover panel with a scrollable jobs list, states, app estimates,
  and pause/resume admission control. Kept the four metrics and settings UI.
- Added utility setup/usage documentation and removed the excluded connection-diagnostic scope
  from the accepted plan. The full staged plan still includes future work; current limitations
  are documented explicitly in `resource-coordination.md`.
- Verification: normal and ASan/UBSan builds each pass all six tests, including two independent
  MCP clients sharing one service. C++ formatting passes. The app stays running in its 10-second
  smoke test. The UI tool exposed only the transparent hover-catcher window, so the expanded
  layout was not visually verified on the Mac.
- A live runner smoke test initially remained queued under system pressure and exposed a queue
  admission issue after the test client exited. Fixed admission to require an owning-client
  request/poll, added a regression check, and cleared the isolated test reservation after
  confirming no test command survived. A subsequent registered `/usr/bin/true` command completed
  through `hm-run` with exit status 0. Sustained workload calibration is still future work.

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

- Set up CI and releases with GitHub Actions, and started the docs structure.
  - `ci.yml` checks formatting, builds and tests in Debug and Release, launches the app, and runs
    the tests under ASan with UBSan and under TSan. `release.yml` publishes a GitHub prerelease
    when a `vX.Y.Z` tag matching the CMake version is pushed. Dependabot updates the pinned actions.
  - Added `scripts/smoke_test.sh`, CMake presets for the sanitizers, and an `hm_tests` target.
  - Added `doc/README.md` as the index, with `architecture.md`, `development.md`, and `ci-cd.md`.
  - Fixed a bug found while checking the package: the app was built for the host's macOS (minimum
    27.0) rather than 14.0, because `swiftc` ignores `CMAKE_OSX_DEPLOYMENT_TARGET`. The new target
    compiled with no availability errors, but the app has not been run on a macOS older than 27.
  - Formatted the 4 lines that already failed clang-format, in `bridge.cpp`, `collectors.cpp`, and
    `sampler_test.cpp`.
- Verified locally, from a clean copy of the working tree on macOS 27 with Swift 6.4:
  - Formatting passes. All four tests pass in Debug and Release, and under both sanitizer presets.
    With every core loaded, they also passed 5 repeats each in TSan and Release.
  - Debug and Release apps each stayed running for 15 seconds, and the smoke test fails on an app
    that exits. A zipped and unpacked build still passed `codesign --verify --deep --strict`.
  - `actionlint` reports no problems in either workflow.
- Not verified: neither workflow has run on GitHub, because nothing has been pushed.
  The first run will also be the first build with Xcode 26.6 on `macos-26`, which is older than the
  local toolchain. The Publish step has not been run, and no tag or release exists.

- Documented a proposed MCP resource coordination service with shared reservations, agent
  registration, pressure safeguards, managed execution, and a staged validation plan. This is
  planning only; no MCP server or resource controls were implemented or tested.

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
