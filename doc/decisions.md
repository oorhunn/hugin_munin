# Decisions

## Decided

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-11-08 | The clipboard preview is a window in the middle of the screen, the notch shows no clipboard content, and the clipboard's controls live in a Clipboard section of Settings. This replaces the notch preview and history list, and adds a third UI surface. | Requested by the owner, who asked for Flycut's placement. |
| 2026-11-08 | The clipboard history, text and images, is saved between launches by default, unencrypted, in an owner-only file in Application Support. Settings can turn it off, which deletes the file. It saves after each change, and Clear deletes it at once. This replaces the session-only history. | Requested by the owner, as in Flycut. Saving after each change survives a crash, and the copy happens on the watcher thread. Copies marked private are never recorded, so they never reach the file. |
| 2026-11-08 | The clipboard follows Flycut's default behavior. Repeating the shortcut walks the history, the stack position persists until a new copy, pasting does not reorder, a repeat of an older entry is a new entry, Return and row clicks paste, and Flycut's default private pasteboard types are skipped. This replaces the earlier move-to-front, return-to-copy, and start-at-newest behavior. Scope is the default settings only. | Requested by the owner. Behavior was taken from Flycut's published source, because macOS denied reading the owner's own Flycut preferences. The preview stays in the notch at Flycut's bezel size, because the notch and Settings remain the only UI surfaces. |
| 2026-11-08 | Releasing all shortcut modifiers commits the preview and pastes to the original app, subject to Accessibility permission. The preview background is 70% opaque. | Requested by the user. Permission is requested explicitly in Settings; missing permission falls back to copying. The previous manual-paste-only keyboard behavior is superseded; Escape still cancels. |
| 2026-11-08 | Cmd+Shift+V opens a focused clipboard preview in the notch; left/right browse, Return restores, Escape dismisses. This replaces the former Cmd+Shift+C default; custom shortcuts are retained. | Requested before merging the clipboard feature. A key-capable nonactivating panel receives arrows without global keyboard monitoring or Accessibility access. |
| 2026-11-06 | Add MCP coordination and command utilities to the app, with one shared per-user authority and app-supplied job estimates in the notch hover panel. | Owner accepted the resource coordination plan and requested job visibility. The first implementation uses conservative advisory reservations. |
| 2026-11-05 | C++23 is the main language. | Set by the project owner. |
| 2026-11-05 | macOS is the first target and Ubuntu follows. | Set by the project owner. Platform code stays isolated so that Ubuntu can be added later. |
| 2026-11-05 | The UI surfaces are the notch UI, the settings menu UI, and the disk tracker window. | Set by the project owner, who asked for the disk tracker to have a separate window. |
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
| 2026-11-06 | `.clang-tidy` enables the bug-finding and modernization check groups, turns off style-only checks, and fails CI on any warning. | Style checks gave most of the 812 findings in a first run. With only the useful checks left, a warning should stop a merge. Proposed during the setup, and the owner asked for it to run in CI. |

| 2026-11-06 | The notch file shelf holds session-only path references, with AppKit file drags and C++ membership logic. | Originals stay in place until the destination handles a move/copy. Successful exports clear entries; cancelled drags retain them. No extra UI surface or storage copy is needed. |
| 2026-11-09 | The file shelf is hidden while empty, and the expanded panel shrinks with it. | An empty shelf only took up space. Dropping on the notch still adds the first item, at the cost of no longer showing a hint about it. |
| 2026-11-05 | The disk tracker compares saved scans rather than watching the file system live. | The question is what changed over days, and periodic scans answer it across reboots and while the app is closed. Proposed during the build. |
| 2026-11-05 | Scans record folders of 10 MB or more and files of 50 MB or more, in text files of about 200 KB. | Keeps 90 days of history small and needs no database, at the cost of an error of up to the limit when something crosses it. Proposed during the build. |
| 2026-11-05 | A scan runs only after the user asks for the first one, then every 6 hours. | Walking the home folder raises the system's privacy prompts, which should not appear unprompted at launch. Proposed during the build. |
| 2026-11-05 | Folders that either scan could not read leave the comparison, and their size leaves their parents. | Losing access would otherwise read as a deletion, and gaining it as growth. Proposed during the build. |
| 2026-11-05 | The growth summary stops at a project, meaning a folder with a `.git` entry, and at a folder whose children do not explain at least half of its growth. | The owner asked which projects or folders used the space, and nested rows would count it more than once. Proposed during the build. |
| 2026-11-05 | The Swift UI saves the list of folders to scan in `UserDefaults`. | It follows the rule for other preferences. Proposed during the build. |
| 2026-11-05 | The disk tracker window only reads. It can show an item in Finder but not delete it. | Deleting is a separate feature with its own risks, and it was not requested. Proposed during the build. |
| 2026-11-06 | The expanded notch panel has two slides, System and Disk, moved between by a swipe or by labels in its footer. | Set by the project owner, who asked to see the disk summary there. The panel keeps its size, and the slide uses the same report and period as the window. |
| 2026-11-06 | The slides are positioned by an offset from the current page, not a paging `ScrollView`. | A `ScrollView` rebuilt on each expand came to rest about 3 points out of place, and showed the wrong slide after a reopen. Found during the build. |

## Open questions

- **Should releases be signed with a Developer ID and notarized?** macOS blocks the app after
  download until they are. This needs a paid Apple Developer Program membership and certificate
  secrets in the repository, so it is the owner's decision.

## Postponed

- **What replaces the notch on Ubuntu?** Ubuntu has no notch equivalent, so that UI needs its own
  design, such as a top bar indicator.
- **How does it behave on Macs without a notch?** This includes external displays.
