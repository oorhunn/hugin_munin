# Architecture

Hugin & Munin samples system metrics in a C++23 core and shows them in a thin SwiftUI app. All
logic stays in the core, and the UI only displays state and collects settings.

```
ui/macos/ (SwiftUI)                   notch UI and settings menu UI
    |  HMBridge module
core/bridge/ (C header)               the only interface Swift can see
    |
core/src/sampler.cpp                  periodic sampling on a background thread
core/src/metrics.cpp                  platform-neutral calculations
    |  core/include/hm/collectors.hpp, the platform boundary
core/src/platform/<os>/collectors.cpp one backend per OS
```

## Targets

| CMake target | Contents |
| --- | --- |
| `hm_core` | Metrics, sampler, and the backend for the current OS |
| `hm_bridge` | The C interface over `hm_core` |
| `HuginMunin` | The macOS app bundle, built to `build/HuginMunin.app` |
| `hm_tests` | Builds the five test programs without building the app |

## How a sample reaches the screen

1. `hm::Sampler` runs on its own thread and calls the collectors.
2. `hm::SampleTracker` turns the readings into an `hm::Sample`. CPU usage needs two readings, and a
   failed memory or disk read keeps the previous value. GPU usage is never stale: it becomes
   unknown instead.
3. Disk is read every 60 seconds by default, because it costs far more than the other collectors.
4. The bridge converts the sample to the C `hm_sample`, where unknown percentages are NaN.
5. `Monitor.swift` receives it on the sampling thread and publishes it on the main actor.

The Swift callback must stay outside the main actor. A closure written inside `Monitor` inherits
that isolation and traps when the core calls it from the sampling thread.

## Platform boundary

`core/include/hm/collectors.hpp` is the only interface a backend implements. Collectors return
`std::expected`, because a failed read is an ordinary outcome for code that samples repeatedly.

`Sources` in `sampler.hpp` holds the collectors as `std::function`s, so that `sampler_test` can
replace them with fakes. Together with the injected clock in `SampleTracker`, this means the
sampling logic needs no real system to test.

To add Ubuntu, implement `collectors.hpp` under `core/src/platform/linux/` and select it in
`CMakeLists.txt`. No macOS type may appear outside `core/src/platform/macos/` and `ui/macos/`.

## File shelf

`hm::FileShelf` in `core/include/hm/file_shelf.hpp` keeps an ordered, deduplicated list of
absolute paths for the current session, exposed through the C bridge. It never performs
filesystem operations. Shelf mutations happen on the UI thread.

`FileShelf.swift` adapts macOS file pasteboards to the core and renders the shelf inside the
expanded notch. AppKit drop receivers on both the collapsed trigger and expanded panel accept
file URLs, including folders. Dragging over the collapsed trigger opens the panel. The drag
source offers copy and move to other applications; the destination chooses the operation using
normal macOS rules. A completed copy or move removes the reference, while cancellation retains it.
The panel stays expanded during an outgoing drag. Entries can also be removed without deleting
anything. Paths are not persisted or tracked after an external rename/move; an unavailable item
cannot be dragged and can be dismissed with its × button.
