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

## Open questions

None right now.

## Postponed

- **What replaces the notch on Ubuntu?** Ubuntu has no notch equivalent, so that UI needs its own
  design, such as a top bar indicator.
- **How does it behave on Macs without a notch?** This includes external displays.
