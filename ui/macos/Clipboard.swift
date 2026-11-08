import AppKit
import Foundation
import HMBridge
import Observation

/// One remembered copy, ready to display: a preview for text, a decoded image for images.
struct ClipboardItem: Identifiable {
    let id: UInt64
    let isImage: Bool
    let preview: String
    let capturedAt: Date
    let size: Int
    let image: NSImage?
}

/// Runs on the core's watcher thread, so it must stay outside the main actor. A closure written
/// inside `ClipboardStore` would inherit that isolation and trap when the core calls it.
private func receiveClipboardChange(_ context: UnsafeMutableRawPointer?) {
    guard let context else { return }
    let store = Unmanaged<ClipboardStore>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor in store.reload() }
}

/// Publishes the core's clipboard history on the main actor. Restoring an entry writes it back
/// to the clipboard; the user pastes it with Cmd+V.
@MainActor @Observable
final class ClipboardStore {
    private(set) var entries: [ClipboardItem] = []
    var previewing = false
    private var selectedID: UInt64?

    var selectedIndex: Int { entries.firstIndex { $0.id == selectedID } ?? 0 }
    var selectedItem: ClipboardItem? { entries.isEmpty ? nil : entries[selectedIndex] }

    func beginPreview() {
        selectedID = entries.first?.id
        previewing = true
    }

    func movePreview(_ offset: Int) {
        guard !entries.isEmpty else { return }
        let index = min(max(selectedIndex + offset, 0), entries.count - 1)
        selectedID = entries[index].id
    }

    func restoreSelected() {
        if let item = selectedItem { restore(item.id) }
    }

    @ObservationIgnored private var clipboard: OpaquePointer?
    /// Decoding an image is expensive, and ids are stable, so each image decodes once.
    @ObservationIgnored private var images: [UInt64: NSImage] = [:]

    /// The store must stay alive until `stop` returns, since the core holds it unretained.
    func start(capacity: Int) {
        guard clipboard == nil else { return }
        clipboard = hm_clipboard_start(receiveClipboardChange,
                                       Unmanaged.passUnretained(self).toOpaque(), capacity)
        reload()
    }

    func setCapacity(_ capacity: Int) {
        hm_clipboard_set_capacity(clipboard, capacity)
        reload()
    }

    /// Puts the entry back on the clipboard and moves it to the front of the history.
    func restore(_ id: UInt64) {
        _ = hm_clipboard_restore(clipboard, id)
        reload()
    }

    func clear() {
        hm_clipboard_clear(clipboard)
        reload()
    }

    func stop() {
        hm_clipboard_stop(clipboard)
        clipboard = nil
    }

    /// Reads one consistent snapshot from the core and turns it into display items.
    func reload() {
        guard let clipboard else { return }
        var count = 0
        guard let snapshot = hm_clipboard_snapshot(clipboard, &count), count > 0 else {
            entries = []
            images.removeAll()
            return
        }
        defer { hm_clipboard_free_snapshot(snapshot, count) }

        var items: [ClipboardItem] = []
        items.reserveCapacity(count)
        var live = Set<UInt64>()
        for i in 0..<count {
            let entry = snapshot[i]
            let id = entry.id
            live.insert(id)
            var image: NSImage?
            if entry.kind == HM_CLIPBOARD_IMAGE, let bytes = entry.image {
                if let cached = images[id] {
                    image = cached
                } else if let decoded = NSImage(data: Data(bytes: bytes, count: entry.image_size)) {
                    image = decoded
                }
                if let image { images[id] = image }
            }
            items.append(ClipboardItem(
                id: id,
                isImage: entry.kind == HM_CLIPBOARD_IMAGE,
                preview: entry.text != nil ? String(cString: entry.text) : "",
                capturedAt: Date(timeIntervalSince1970: entry.captured_at),
                size: entry.size,
                image: image))
        }
        images = images.filter { live.contains($0.key) }
        entries = items
    }
}
