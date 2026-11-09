import AppKit
import Carbon.HIToolbox
import HMBridge
import SwiftUI

/// The centered preview window. It never activates the app, and ordering it out hands the keyboard
/// back to the app the user is working in.
@MainActor
private final class BezelPanel: NSPanel {
    var onKey: ((NSEvent) -> Void)?
    var onScroll: ((CGFloat) -> Void)?
    var onDoubleClick: (() -> Void)?
    var onModifiersReleased: (() -> Void)?
    var onDismiss: (() -> Void)?

    override var canBecomeKey: Bool { true }

    override func sendEvent(_ event: NSEvent) {
        switch event.type {
        case .keyDown:
            onKey?(event)
        case .scrollWheel:
            onScroll?(event.deltaY)
        case .leftMouseUp where event.clickCount == 2:
            onDoubleClick?()
        case .flagsChanged:
            if event.modifierFlags.intersection([.command, .shift, .option, .control]).isEmpty {
                onModifiersReleased?()
            }
        default:
            super.sendEvent(event)
        }
    }

    override func resignKey() {
        super.resignKey()
        onDismiss?()
    }
}

/// Shows the clipboard stack in the middle of the screen and turns input into stack moves.
@MainActor
final class ClipboardBezelController {
    private static let size = CGSize(width: 500, height: 320)

    private let panel: BezelPanel
    private let pasteDelivery: ClipboardPasteDelivery
    private var releaseTimer: Timer?
    private let session: ClipboardPreviewSession

    init(store: ClipboardStore, settings: Settings) {
        let delivery = ClipboardPasteDelivery()
        pasteDelivery = delivery
        session = ClipboardPreviewSession(store: store, settings: settings, paste: { delivery.paste() })
        panel = BezelPanel(contentRect: NSRect(origin: .zero, size: Self.size),
                           styleMask: [.borderless, .nonactivatingPanel],
                           backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.level = .screenSaver
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.hidesOnDeactivate = false
        panel.isMovable = false
        let hosting = NSHostingView(rootView: ClipboardBezelView(store: store))
        hosting.sizingOptions = []
        panel.contentView = hosting

        session.onClose = { [weak self] in self?.closePanel() }
        panel.onKey = { [weak self] event in self?.session.handleKey(event) }
        panel.onScroll = { [weak self] delta in self?.session.handleScroll(delta) }
        panel.onDoubleClick = { [weak self] in self?.session.commit() }
        panel.onModifiersReleased = { [weak self] in self?.session.commit() }
        panel.onDismiss = { [weak self] in self?.session.cancel() }
    }

    /// Opens the preview, or steps to the next older entry if it is already open, so holding the
    /// modifiers and tapping the key walks the history as in Flycut.
    func open() {
        guard session.open() else { return }
        pasteDelivery.rememberDestination()
        if let screen = NSScreen.main ?? NSScreen.screens.first {
            let area = screen.visibleFrame
            panel.setFrameOrigin(NSPoint(x: area.midX - Self.size.width / 2,
                                         y: area.midY - Self.size.height / 2))
        }
        panel.makeKeyAndOrderFront(nil)
        // Also catches a fast release that happened before the panel became key.
        releaseTimer = Timer.scheduledTimer(withTimeInterval: 0.04, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard NSEvent.modifierFlags.intersection([.command, .shift, .option, .control]).isEmpty
                else { return }
                self?.session.commit()
            }
        }
    }

    private func closePanel() {
        releaseTimer?.invalidate()
        releaseTimer = nil
        panel.orderOut(nil)
    }
}

/// One entry, its place in the stack, and when it was copied.
private struct ClipboardBezelView: View {
    let store: ClipboardStore

    var body: some View {
        VStack(spacing: 8) {
            Group {
                if let image = store.selectedItem?.image {
                    Image(nsImage: image).resizable().scaledToFit()
                } else {
                    Text(store.selectedItem?.preview ?? "")
                        .font(.system(size: 13))
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipped()
            Text(store.entries.isEmpty ? "Empty" : "\(store.position + 1) of \(store.entries.count)")
                .font(.system(size: 13, weight: .semibold, design: .rounded))
                .monospacedDigit()
            if let item = store.selectedItem {
                Text(item.capturedAt.formatted(date: .abbreviated, time: .shortened))
                    .font(.caption).foregroundStyle(.secondary)
            }
            if !ClipboardPasteDelivery.isAllowed {
                Text("Release to copy. Allow Accessibility in Settings to paste automatically.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(.black.opacity(0.70), in: RoundedRectangle(cornerRadius: 18))
        .foregroundStyle(.white)
        .environment(\.colorScheme, .dark)
    }
}
