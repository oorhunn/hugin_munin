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

    private let store: ClipboardStore
    private let settings: Settings
    private let panel: BezelPanel
    private let pasteDelivery = ClipboardPasteDelivery()
    private var releaseTimer: Timer?
    private var isOpen = false

    init(store: ClipboardStore, settings: Settings) {
        self.store = store
        self.settings = settings
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

        panel.onKey = { [weak self] event in self?.handleKey(event) }
        panel.onScroll = { [weak self] delta in self?.handleScroll(delta) }
        panel.onDoubleClick = { [weak self] in self?.commit() }
        panel.onModifiersReleased = { [weak self] in self?.commit() }
        panel.onDismiss = { [weak self] in self?.close() }
    }

    /// Opens the preview, or steps to the next older entry if it is already open, so holding the
    /// modifiers and tapping the key walks the history as in Flycut.
    func open() {
        guard !isOpen else {
            store.move(HM_CLIPBOARD_STEP_OLDER)
            return
        }
        isOpen = true
        pasteDelivery.rememberDestination()
        store.reload()
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
                self?.commit()
            }
        }
    }

    private func close() {
        guard isOpen else { return }
        isOpen = false
        releaseTimer?.invalidate()
        releaseTimer = nil
        panel.orderOut(nil)
    }

    /// Puts the selected entry on the clipboard, closes the preview, and pastes it.
    private func commit() {
        guard isOpen else { return }
        let restored = store.restoreSelected()
        close()
        if restored { pasteDelivery.paste() }
    }

    private func handleScroll(_ delta: CGFloat) {
        guard isOpen, delta != 0 else { return }
        store.move(delta > 0 ? HM_CLIPBOARD_STEP_NEWER : HM_CLIPBOARD_STEP_OLDER)
    }

    /// Flycut's bezel keys, matched by key code so they still work with Shift held.
    private func handleKey(_ event: NSEvent) {
        guard isOpen else { return }
        let code = Int(event.keyCode)
        if UInt32(code) == settings.shortcut.keyCode {
            store.move(event.modifierFlags.contains(.shift)
                ? HM_CLIPBOARD_STEP_NEWER : HM_CLIPBOARD_STEP_OLDER)
            return
        }
        switch code {
        case kVK_Escape: close()
        case kVK_Return: commit()
        case kVK_ANSI_KeypadEnter:
            store.restoreSelected()
            close()
        case kVK_UpArrow, kVK_LeftArrow, kVK_ANSI_K: store.move(HM_CLIPBOARD_STEP_NEWER)
        case kVK_DownArrow, kVK_RightArrow, kVK_ANSI_J: store.move(HM_CLIPBOARD_STEP_OLDER)
        case kVK_Home: store.move(HM_CLIPBOARD_STEP_NEWEST)
        case kVK_End: store.move(HM_CLIPBOARD_STEP_OLDEST)
        case kVK_PageUp: store.move(HM_CLIPBOARD_STEP_TEN_NEWER)
        case kVK_PageDown: store.move(HM_CLIPBOARD_STEP_TEN_OLDER)
        case kVK_Delete: store.removeSelected()
        default:
            if let index = Self.digitKeys.firstIndex(of: code) { store.jump(to: index) }
        }
    }

    /// Number keys in stack order, where 0 is the tenth entry.
    private static let digitKeys = [kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
                                    kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9, kVK_ANSI_0]
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
