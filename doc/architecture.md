# Architecture

Hugin & Munin samples system metrics in a shared C++23 service and shows them in a thin SwiftUI app. All
logic stays in the core, and the UI only displays state and collects settings.

```
SwiftUI notch/settings -> C bridge background IPC client
MCP client -> hm-mcp stdio adapter -> private Unix socket -> hm-service
Local commands -> hm-run --------> private Unix socket -> hm-service
hm-service -> Coordinator (registrations, policy, leases, queue)
           -> Sampler -> platform collectors
Clipboard watcher -> platform pasteboard backend (independent of resource service)
```

The per-user service is the single sampler and reservation authority. The bridge polls its
snapshot off the main thread. See [resource coordination](resource-coordination.md) for lifecycle,
policy, protocol, and limitations. JSON uses vendored nlohmann/json 3.12.0 (MIT license in
`third_party/nlohmann/LICENSE.MIT`).

## Targets

| CMake target | Contents |
| --- | --- |
| `hm_core` | Metrics, sampler, coordinator, IPC, clipboard history, and the backend for the current OS |
| `hm-service`, `hm-mcp`, `hm-run` | Shared service, stdio MCP adapter, and registered command runner |
| `hm_bridge` | The C interface over `hm_core` |
| `HuginMunin` | The macOS app bundle, built to `build/HuginMunin.app` |
| `hm_tests` | Builds seven C++ test programs and the MCP utilities without building the app |

## How a sample reaches the screen

1. `hm::Sampler` in the service runs on its own thread and calls the collectors.
2. `hm::SampleTracker` turns the readings into an `hm::Sample`. CPU usage needs two readings, and a
   failed memory or disk read keeps the previous value. GPU usage is never stale: it becomes
   unknown instead.
3. Disk is read every 60 seconds by default, because it costs far more than the other collectors.
4. The bridge reads the service snapshot and converts it to `hm_sample`, including a callback-scoped JSON payload; unknown percentages are NaN.
5. `Monitor.swift` receives it on the sampling thread and publishes it on the main actor.

The Swift callback must stay outside the main actor. A closure written inside `Monitor` inherits
that isolation and traps when the core calls it from the sampling thread. The clipboard change
callback follows the same rule in `ClipboardStore`.

## Platform boundary

`core/include/hm/collectors.hpp` and `core/include/hm/pasteboard.hpp` are the interfaces a
backend implements. Collectors return `std::expected`, because a failed read is an ordinary
outcome for code that samples repeatedly. The pasteboard boundary exposes the change counter and
read/write functions, which is all the watcher needs.

`Sources` in `sampler.hpp` and `PasteboardSources` in `clipboard.hpp` hold these functions as
`std::function`s, so `sampler_test` and `clipboard_test` can replace them with fakes. Together
with the injected clock in `SampleTracker`, this means the sampling and clipboard logic need no
real system to test.
Freshness flags distinguish retained memory/disk values from successful collector reads. Memory
pressure is optional and never retained after a failure.

To add Ubuntu, implement both headers under `core/src/platform/linux/` and select the files in
`CMakeLists.txt`. No macOS type may appear outside `core/src/platform/macos/` and `ui/macos/`.

## How a copy reaches the screen

1. `hm::ClipboardWatcher` polls the pasteboard change counter on its own thread, every 0.5 s.
2. When the counter moves, `hm::ClipboardTracker` reads the pasteboard. An empty read is retried
   once on the next poll, because a poll can land inside another process's write, between the
   clear and the new contents.
3. `hm::ClipboardHistory` records the content, ignoring a repeat of the newest entry. The entry
   count and total payload bytes both stay within their limits, evicting the oldest. Content that
   password managers mark as private never gets this far, because `read_pasteboard` rejects it.
4. The bridge turns the history into a malloc'd `hm_clipboard_entry` array with a 4095-byte text
   preview, which `ClipboardStore.swift` copies into Swift values on the main actor.
5. The global shortcut, registered in `HotKey.swift` with Carbon's `RegisterEventHotKey`, opens
   the preview in `ClipboardBezel.swift`, a nonactivating panel centered on the screen. Ordering
   it out returns focus to the app the user was working in.
6. `hm::ClipboardWatcher` owns the stack position, which persists between previews and returns to
   the newest entry on an external copy. Choosing an entry writes it back to the pasteboard
   without reordering the history, and `ClipboardPasteDelivery` then sends Cmd+V.

The history is saved when Settings allow it. The Swift UI chooses the path, which is
`~/Library/Application Support/HuginMunin/clipboard-history.bin`, and the core does the rest: it
loads the file before the watcher starts and saves on the watcher thread after each change,
writing a temporary owner-only file and renaming it over the old one. A damaged file is ignored.
`clear` and turning saving off delete the file straight away, and `save_mutex_` stops a save in
progress from bringing it back.

## File shelf

`hm::FileShelf` in `core/include/hm/file_shelf.hpp` keeps an ordered, deduplicated list of
absolute paths for the current session, exposed through the C bridge. It never performs
filesystem operations. Shelf mutations happen on the UI thread.

`FileShelf.swift` adapts macOS file pasteboards to the core and renders the shelf inside the
expanded notch. The shelf is hidden while empty, and the panel and its window shrink to fit the
metrics, so dropping on the notch is how the first item arrives. AppKit drop receivers on both the collapsed trigger and expanded panel accept
file URLs, including folders. Dragging over the collapsed trigger opens the panel. The drag
source offers copy and move to other applications; the destination chooses the operation using
normal macOS rules. A completed copy or move removes the reference, while cancellation retains it.
The panel stays expanded during an outgoing drag. Entries can also be removed without deleting
anything. Paths are not persisted or tracked after an external rename/move; an unavailable item
cannot be dragged and can be dismissed with its × button.
