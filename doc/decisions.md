# Decisions

## Decided

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-11-05 | C++23 is the main language. | Set by the project owner. |
| 2026-11-05 | macOS is the first target and Ubuntu follows. | Set by the project owner. Platform code stays isolated so that Ubuntu can be added later. |
| 2026-11-05 | The only UI surfaces are the notch UI and the settings menu UI. | Set by the project owner. |
| 2026-11-05 | `doc/` is updated with each accomplishment. | Set by the project owner. |
| 2026-11-05 | `CLAUDE.md` holds the instructions and `AGENTS.md` points to it. | Set by the project owner. |
| 2026-11-05 | CPU, memory, and disk usage are the first metrics, reusing `resource_visualizer` where possible. | Set by the project owner. |
| 2026-11-05 | The UI uses SwiftUI. | Set by the project owner. |
| 2026-11-05 | CMake is the build system. | Set by the project owner. |
| 2026-11-05 | Collectors return `std::expected` rather than throwing. | The caller samples repeatedly and a failed read is an ordinary outcome. Proposed during the port. |
| 2026-11-05 | Tests use a small `require` helper instead of a test framework. | Keeps the project free of dependencies for now. Proposed during the port. |
| 2026-11-05 | Swift calls the core through a small C header. | Set by the project owner. It already works in `resource_visualizer`. |
| 2026-11-05 | CMake builds the SwiftUI app too. | Set by the project owner, so the project has one build. |
| 2026-11-05 | Settings opens from a button in the expanded notch panel, in a small window. | Set by the project owner. The app has no Dock icon, and a menu bar icon would be a third UI surface. |
| 2026-11-05 | Settings cover the update interval, the metric on each side of the notch, and launch at login. | Set by the project owner. |
| 2026-11-05 | The Swift UI saves preferences in `UserDefaults`, and macOS holds the launch at login state. | Preferences are UI state, and the user can also change login items in System Settings. Proposed during the build. |
| 2026-11-05 | The core limits the interval to 0.25 to 10 seconds, and the UI offers 0.5, 1, 2, 5, and 10. | Fixed choices avoid a burst of samples while dragging a slider. Proposed during the build. |
| 2026-11-05 | The notch and its panel show CPU, memory, disk, and GPU side by side. | Set by the project owner. |
| 2026-11-05 | Settings choose which metrics sit beside the notch with one checkbox each, and the notch splits them evenly, with the extra one on the left. This replaces the left and right pickers. | One picker per side cannot show four metrics. Proposed during the build. |
| 2026-11-05 | A failed or missing GPU read shows as unknown instead of keeping the last value. | GPU usage changes quickly, so an old value would be misleading. Proposed during the build. |
| 2026-11-05 | GitHub Actions runs CI and releases, on `macos-26` with one `ubuntu-24.04` job for formatting. | The code is already on GitHub, and macOS runners are free for public repositories. An exact label stops image changes arriving unannounced. Proposed during the CI setup. |
| 2026-11-05 | Pushing a `vX.Y.Z` tag that matches the CMake version publishes a release. | Releases stay deliberate, and the guard stops a tag from shipping a different version number. Proposed during the CI setup. |
| 2026-11-05 | Releases are marked as prereleases until they are properly signed. | Ad hoc builds are blocked by macOS after download, so they are not ready for general use. Proposed during the CI setup. |
| 2026-11-05 | Releases are built for Apple Silicon only. | Every Mac with a notch has an Apple Silicon chip. Proposed during the CI setup. |
| 2026-11-05 | Workflows pin actions to commit SHAs, and Dependabot proposes updates. | A release job has write access, and a tag on an action can be moved. Proposed during the CI setup. |
| 2026-11-05 | `doc/` is split into a guide per topic, with `doc/README.md` as the index. | `progress.md` and `decisions.md` record history, and the new pages describe how things work now. Proposed during the docs setup. |
| 2026-11-05 | The clipboard history keeps the last 50 copies by default, adjustable from 10 to 200 in Settings and clamped to 1–500 in the core, and holds at most 64 MiB of payload in total, evicting the oldest first. A single item over the budget is not recorded. | Bounded memory was a requirement, and two limits keep it true whatever is copied. Proposed during the build. |
| 2026-11-05 | Copying content the history already holds moves that entry to the front instead of adding a duplicate; the newest entry is left alone. | One entry per distinct payload, in most-recently-used order, as other clipboard managers do. Proposed during the build. |
| 2026-11-05 | Restoring an entry writes it back to the pasteboard and moves it to the front; the user then pastes with Cmd+V. | Sending a paste keystroke to other apps requires Accessibility permission. Restore-then-Cmd+V needs none, and the project owner accepted that semantic. |
| 2026-11-05 | The default shortcut is Cmd+Shift+C, editable in Settings, and registered with Carbon's `RegisterEventHotKey`. | Carbon hotkeys need no Accessibility permission, unlike event taps and global monitors. Cmd+Shift+V is Paste and Match Style in many apps, so Cmd+Shift+C (also Maccy's default) conflicts less. A usable shortcut must include Cmd, Option, or Control. |
| 2026-11-05 | The pasteboard is watched by polling its change count every 0.5 s, and a read that comes back empty or fails is retried once before its change count is treated as seen. | There is no event API for other processes' copies without privileges. A poll can land inside another process's write, between the clear and the new data, and the retry covers that window. Found as an intermittent `bridge_test` failure. |
| 2026-11-05 | Text is preferred when the pasteboard offers both text and an image, and images are stored and restored as TIFF, converted from PNG when that is all an app offered. | Text is what a copy usually means and pastes everywhere; TIFF is accepted by every macOS app and needs no re-encode on restore. Proposed during the build. |
| 2026-11-05 | The clipboard history is shown in the expanded notch panel below the metrics, and that panel grew from 112 to 320 points. | The notch UI and Settings are the only surfaces; a clipboard window would be a third. Proposed during the build. |
| 2026-11-05 | The capacity and the shortcut are saved in `UserDefaults` with the other preferences. | Follows the existing Settings pattern. Proposed during the build. |

| 2026-11-06 | The notch file shelf holds session-only path references, with AppKit file drags and C++ membership logic. | Originals stay in place until the destination handles a move/copy. Successful exports clear entries; cancelled drags retain them. No extra UI surface or storage copy is needed. |

## Open questions

- **Should releases be signed with a Developer ID and notarized?** macOS blocks the app after
  download until they are. This needs a paid Apple Developer Program membership and certificate
  secrets in the repository, so it is the owner's decision.

## Postponed

- **What replaces the notch on Ubuntu?** Ubuntu has no notch equivalent, so that UI needs its own
  design, such as a top bar indicator.
- **How does it behave on Macs without a notch?** This includes external displays.
