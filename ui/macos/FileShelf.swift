import AppKit
import HMBridge
import Observation
import OSLog
import SwiftUI

private let shelfLog = Logger(subsystem: "HuginMunin", category: "FileShelf")

@MainActor @Observable
final class FileShelf {
    // Immutable ownership wrapper permits cleanup from Swift's nonisolated deinit.
    private final class Handle: @unchecked Sendable {
        let pointer = hm_file_shelf_create()
        deinit { hm_file_shelf_destroy(pointer) }
    }
    private let storage = Handle()
    private var handle: OpaquePointer? { storage.pointer }
    private(set) var urls: [URL] = []
    /// Lets the notch resize its window when the shelf gains or loses items.
    @ObservationIgnored var onChange: (() -> Void)?

    func add(_ urls: [URL]) {
        for url in urls where url.isFileURL {
            _ = url.path.withCString { hm_file_shelf_add(handle, $0) }
        }
        refresh()
    }

    func remove(_ url: URL) {
        url.path.withCString { hm_file_shelf_remove(handle, $0) }
        refresh()
    }

    private func refresh() {
        urls = (0..<hm_file_shelf_count(handle)).compactMap {
            hm_file_shelf_path(handle, $0).map { URL(fileURLWithPath: String(cString: $0)) }
        }
        onChange?()
    }
}

/// AppKit receives file drags even while the collapsed notch is waiting for a hover.
@MainActor
final class ShelfDropHosting: NSHostingView<AnyView> {
    var onEnter: (() -> Void)?
    var onExit: (() -> Void)?
    var onDrop: (([URL]) -> Void)?

    required init(rootView: AnyView) {
        super.init(rootView: rootView)
        registerForDraggedTypes([.fileURL])
    }

    @available(*, unavailable)
    required init(coder: NSCoder) { fatalError() }

    private func urls(_ sender: any NSDraggingInfo) -> [URL] {
        // A source inside this app is the shelf's own outgoing drag. Accepting it would report a
        // completed copy to the source, which then removes the entry although nothing moved.
        guard sender.draggingSource == nil else { return [] }
        return (sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
            options: [.urlReadingFileURLsOnly: true]) as? [URL]) ?? []
    }

    override func draggingEntered(_ sender: any NSDraggingInfo) -> NSDragOperation {
        guard !urls(sender).isEmpty else { return [] }
        onEnter?()
        return .copy
    }

    override func draggingUpdated(_ sender: any NSDraggingInfo) -> NSDragOperation {
        urls(sender).isEmpty ? [] : .copy
    }

    override func draggingExited(_ sender: (any NSDraggingInfo)?) { onExit?() }

    override func prepareForDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        !urls(sender).isEmpty
    }

    override func performDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        let files = urls(sender)
        guard !files.isEmpty else { return false }
        onDrop?(files)
        return true
    }

    override func concludeDragOperation(_ sender: (any NSDraggingInfo)?) { onExit?() }
}

struct FileShelfView: View {
    let state: NotchState
    private var shelf: FileShelf { state.shelf }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("File shelf", systemImage: "tray")
                Spacer()
                Text("\(shelf.urls.count) items").foregroundStyle(.secondary)
            }
            .font(.caption)
            ScrollView(.horizontal) {
                HStack(spacing: 10) {
                    ForEach(shelf.urls, id: \.self) { url in
                        HStack(spacing: 2) {
                            ShelfDragItem(url: url, state: state,
                                          completed: { shelf.remove(url) })
                                .frame(width: 145, height: 54)
                            Button { shelf.remove(url) } label: {
                                Image(systemName: "xmark.circle.fill")
                            }
                            .buttonStyle(.borderless)
                            .help("Remove from shelf (keeps the original)")
                        }
                        .padding(6)
                        .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                    }
                }
            }
            .frame(height: 78)
        }
        .padding(10)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 10))
    }
}

private struct ShelfDragItem: NSViewRepresentable {
    let url: URL
    let state: NotchState
    let completed: @MainActor () -> Void

    func makeNSView(context: Context) -> ShelfDragView { ShelfDragView() }
    func updateNSView(_ view: ShelfDragView, context: Context) {
        view.url = url
        view.state = state
        view.completed = completed
        view.toolTip = url.path
        view.setAccessibilityLabel("Drag \(url.lastPathComponent)")
        view.needsDisplay = true
    }
}

private final class ShelfDragView: NSView, NSDraggingSource {
    var url = URL(fileURLWithPath: "/")
    // Share the main-actor state directly: passing a Bool callback through the
    // representable triggers a compiler IR-generation crash on Swift 6.3.
    weak var state: NotchState?
    var completed: (@MainActor () -> Void)?

    override func draw(_ dirtyRect: NSRect) {
        NSWorkspace.shared.icon(forFile: url.path).draw(in: NSRect(x: 4, y: 14, width: 26, height: 26))
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingMiddle
        (url.lastPathComponent as NSString).draw(
            in: NSRect(x: 36, y: 16, width: bounds.width - 40, height: 22),
            withAttributes: [.font: NSFont.systemFont(ofSize: 11),
                             .foregroundColor: NSColor.white, .paragraphStyle: paragraph])
    }

    override func mouseDown(with event: NSEvent) {}
    override func mouseDragged(with event: NSEvent) {
        guard FileManager.default.fileExists(atPath: url.path) else {
            shelfLog.notice("Drag refused, file not found: \(self.url.path, privacy: .private) (.\(self.url.pathExtension, privacy: .public))")
            NSSound.beep()
            return
        }
        let item = NSDraggingItem(pasteboardWriter: url as NSURL)
        item.setDraggingFrame(bounds, contents: NSWorkspace.shared.icon(forFile: url.path))
        state?.dragging = true
        beginDraggingSession(with: [item], event: event, source: self)
    }

    func draggingSession(_ session: NSDraggingSession,
                         sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        context == .outsideApplication ? [.copy, .move] : []
    }

    func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint,
                         operation: NSDragOperation) {
        shelfLog.notice("Drag of .\(self.url.pathExtension, privacy: .public) ended with operation \(operation.rawValue, privacy: .public)")
        if !operation.intersection([.copy, .move]).isEmpty { completed?() }
        state?.dragging = false
    }
}
