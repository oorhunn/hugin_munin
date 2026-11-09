import AppKit
import Carbon.HIToolbox
import HMBridge

@MainActor
protocol ClipboardSelection: AnyObject {
    func reload()
    func move(_ step: hm_clipboard_step)
    func jump(to position: Int)
    @discardableResult func restoreSelected() -> Bool
    func removeSelected()
}

extension ClipboardStore: ClipboardSelection {}

/// Input behavior shared by the panel and tests. Closing first makes repeated release events inert.
@MainActor
final class ClipboardPreviewSession {
    private let store: any ClipboardSelection
    private let settings: Settings
    private let paste: () -> Void
    var onClose: (() -> Void)?
    private(set) var isOpen = false

    init(store: any ClipboardSelection, settings: Settings, paste: @escaping () -> Void) {
        self.store = store
        self.settings = settings
        self.paste = paste
    }

    /// Returns true only when a new panel should be shown.
    @discardableResult
    func open() -> Bool {
        if isOpen {
            store.move(HM_CLIPBOARD_STEP_OLDER)
            return false
        }
        isOpen = true
        store.reload()
        return true
    }

    func cancel() {
        guard isOpen else { return }
        isOpen = false
        onClose?()
    }

    func commit() {
        guard isOpen else { return }
        let restored = store.restoreSelected()
        cancel()
        if restored { paste() }
    }

    func handleScroll(_ delta: CGFloat) {
        guard isOpen, delta != 0 else { return }
        store.move(delta > 0 ? HM_CLIPBOARD_STEP_NEWER : HM_CLIPBOARD_STEP_OLDER)
    }

    /// Flycut's bezel keys, matched by key code so they still work with Shift held.
    func handleKey(_ event: NSEvent) {
        guard isOpen else { return }
        let code = Int(event.keyCode)
        if UInt32(code) == settings.shortcut.keyCode {
            store.move(event.modifierFlags.contains(.shift)
                ? HM_CLIPBOARD_STEP_NEWER : HM_CLIPBOARD_STEP_OLDER)
            return
        }
        switch code {
        case kVK_Escape: cancel()
        case kVK_Return: commit()
        case kVK_ANSI_KeypadEnter:
            store.restoreSelected()
            cancel()
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
