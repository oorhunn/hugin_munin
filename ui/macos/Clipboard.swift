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
    let image: NSImage?
}

/// Runs on the core's watcher thread, so it must stay outside the main actor. A closure written
/// inside `ClipboardStore` would inherit that isolation and trap when the core calls it.
private func receiveClipboardChange(_ context: UnsafeMutableRawPointer?) {
    guard let context else { return }
    let store = Unmanaged<ClipboardStore>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor in store.reload() }
}

/// Publishes the core's clipboard history and keyboard preview position on the main actor.
@MainActor @Observable
final class ClipboardStore {
    private(set) var entries: [ClipboardItem] = []
    private(set) var position = 0

    var selectedItem: ClipboardItem? { entries.indices.contains(position) ? entries[position] : nil }

    func move(_ step: hm_clipboard_step) { position = hm_clipboard_stack_move(clipboard, step) }
    func jump(to position: Int) { self.position = hm_clipboard_stack_jump(clipboard, position) }

    /// Puts the selected entry on the clipboard, leaving the history order and position alone.
    @discardableResult
    func restoreSelected() -> Bool { hm_clipboard_stack_restore(clipboard) }

    func removeSelected() {
        hm_clipboard_stack_remove(clipboard)
        reload()
    }

    @ObservationIgnored private var clipboard: OpaquePointer?
    /// Decoding an image is expensive, and ids are stable, so each image decodes once.
    @ObservationIgnored private var images: [UInt64: NSImage] = [:]

    /// The store must stay alive until `stop` returns, since the core holds it unretained.
    func start(capacity: Int, historyFile: URL?) {
        guard clipboard == nil else { return }
        let context = Unmanaged.passUnretained(self).toOpaque()
        clipboard = withPath(historyFile) {
            hm_clipboard_start(receiveClipboardChange, context, capacity, $0)
        }
        reload()
    }

    /// Starts saving to `file`, or stops saving and deletes the saved history when it is nil.
    func setHistoryFile(_ file: URL?) {
        _ = withPath(file) { hm_clipboard_set_history_file(clipboard, $0) }
    }

    private func withPath<Result>(_ file: URL?, _ body: (UnsafePointer<CChar>?) -> Result) -> Result {
        file?.withUnsafeFileSystemRepresentation(body) ?? body(nil)
    }

    func setCapacity(_ capacity: Int) {
        hm_clipboard_set_capacity(clipboard, capacity)
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
        position = hm_clipboard_stack_position(clipboard)
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
                image: image))
        }
        images = images.filter { live.contains($0.key) }
        entries = items
    }
}
