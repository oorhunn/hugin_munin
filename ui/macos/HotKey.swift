import AppKit
@preconcurrency import ApplicationServices
import Carbon.HIToolbox

/// A keyboard shortcut, stored as a Carbon key code plus Carbon modifier flags so it can go
/// straight to RegisterEventHotKey and into UserDefaults.
struct HotKey: Equatable {
    var keyCode: UInt32
    var modifiers: UInt32 // Carbon mask: cmdKey, optionKey, controlKey, shiftKey

    /// Opens the clipboard preview. Custom shortcuts remain editable in Settings.
    static let standard = HotKey(keyCode: UInt32(kVK_ANSI_V),
                                 modifiers: UInt32(cmdKey | shiftKey))

    /// A usable shortcut needs a key and at least one of Cmd, Option, or Control, which makes it
    /// a system-wide combination rather than plain typing.
    var isValid: Bool {
        keyCode != 0 &&
            modifiers & UInt32(cmdKey | optionKey | controlKey) != 0
    }

    /// True when this is a plain Escape press, which the recorder treats as cancel.
    var isEscape: Bool { keyCode == UInt32(kVK_Escape) && modifiers == 0 }

    var label: String {
        var parts: [String] = []
        if modifiers & UInt32(controlKey) != 0 { parts.append("⌃") }
        if modifiers & UInt32(optionKey) != 0 { parts.append("⌥") }
        if modifiers & UInt32(shiftKey) != 0 { parts.append("⇧") }
        if modifiers & UInt32(cmdKey) != 0 { parts.append("⌘") }
        parts.append(Self.keyLabel(keyCode))
        return parts.joined()
    }

    /// Carbon modifier flags for the modifiers an NSEvent reports.
    static func carbonModifiers(_ flags: NSEvent.ModifierFlags) -> UInt32 {
        var mask: UInt32 = 0
        if flags.contains(.command) { mask |= UInt32(cmdKey) }
        if flags.contains(.option) { mask |= UInt32(optionKey) }
        if flags.contains(.control) { mask |= UInt32(controlKey) }
        if flags.contains(.shift) { mask |= UInt32(shiftKey) }
        return mask
    }

    private static func keyLabel(_ keyCode: UInt32) -> String {
        let keys: [UInt32: String] = [
            UInt32(kVK_ANSI_A): "A", UInt32(kVK_ANSI_B): "B", UInt32(kVK_ANSI_C): "C",
            UInt32(kVK_ANSI_D): "D", UInt32(kVK_ANSI_E): "E", UInt32(kVK_ANSI_F): "F",
            UInt32(kVK_ANSI_G): "G", UInt32(kVK_ANSI_H): "H", UInt32(kVK_ANSI_I): "I",
            UInt32(kVK_ANSI_J): "J", UInt32(kVK_ANSI_K): "K", UInt32(kVK_ANSI_L): "L",
            UInt32(kVK_ANSI_M): "M", UInt32(kVK_ANSI_N): "N", UInt32(kVK_ANSI_O): "O",
            UInt32(kVK_ANSI_P): "P", UInt32(kVK_ANSI_Q): "Q", UInt32(kVK_ANSI_R): "R",
            UInt32(kVK_ANSI_S): "S", UInt32(kVK_ANSI_T): "T", UInt32(kVK_ANSI_U): "U",
            UInt32(kVK_ANSI_V): "V", UInt32(kVK_ANSI_W): "W", UInt32(kVK_ANSI_X): "X",
            UInt32(kVK_ANSI_Y): "Y", UInt32(kVK_ANSI_Z): "Z",
            UInt32(kVK_ANSI_0): "0", UInt32(kVK_ANSI_1): "1", UInt32(kVK_ANSI_2): "2",
            UInt32(kVK_ANSI_3): "3", UInt32(kVK_ANSI_4): "4", UInt32(kVK_ANSI_5): "5",
            UInt32(kVK_ANSI_6): "6", UInt32(kVK_ANSI_7): "7", UInt32(kVK_ANSI_8): "8",
            UInt32(kVK_ANSI_9): "9",
            UInt32(kVK_Space): "Space", UInt32(kVK_Tab): "Tab",
            UInt32(kVK_Return): "↩", UInt32(kVK_Delete): "⌫",
            UInt32(kVK_LeftArrow): "←", UInt32(kVK_RightArrow): "→",
            UInt32(kVK_UpArrow): "↑", UInt32(kVK_DownArrow): "↓",
            UInt32(kVK_ANSI_Keypad0): "Keypad 0", UInt32(kVK_ANSI_Keypad1): "Keypad 1",
            UInt32(kVK_ANSI_Keypad2): "Keypad 2", UInt32(kVK_ANSI_Keypad3): "Keypad 3",
            UInt32(kVK_ANSI_Keypad4): "Keypad 4", UInt32(kVK_ANSI_Keypad5): "Keypad 5",
            UInt32(kVK_ANSI_Keypad6): "Keypad 6", UInt32(kVK_ANSI_Keypad7): "Keypad 7",
            UInt32(kVK_ANSI_Keypad8): "Keypad 8", UInt32(kVK_ANSI_Keypad9): "Keypad 9",
            UInt32(kVK_F1): "F1", UInt32(kVK_F2): "F2", UInt32(kVK_F3): "F3", UInt32(kVK_F4): "F4",
            UInt32(kVK_F5): "F5", UInt32(kVK_F6): "F6", UInt32(kVK_F7): "F7", UInt32(kVK_F8): "F8",
            UInt32(kVK_F9): "F9", UInt32(kVK_F10): "F10", UInt32(kVK_F11): "F11",
            UInt32(kVK_F12): "F12",
        ]
        return keys[keyCode] ?? "Key \(keyCode)"
    }
}

/// A system-wide keyboard shortcut. Carbon's hot key API is used because it needs no
/// Accessibility permission, unlike event taps and global event monitors. The center lives for
/// the app's lifetime, so it registers no cleanup: the system drops a process's hot keys when it
/// exits.
@MainActor
final class HotKeyCenter {
    var onPressed: (() -> Void)?

    private static let signature = OSType(0x484D434C) // 'HMCL'
    private static let hotKeyId: UInt32 = 1

    private var hotKey: EventHotKeyRef?
    private var handler: EventHandlerRef?

    init() {
        var event = EventTypeSpec(eventClass: UInt32(kEventClassKeyboard),
                                  eventKind: UInt32(kEventHotKeyPressed))
        let status = InstallEventHandler(
            GetApplicationEventTarget(),
            { _, event, userData in
                guard let event, let userData else { return noErr }
                var pressed = EventHotKeyID()
                GetEventParameter(event, EventParamName(kEventParamDirectObject),
                                  EventParamType(typeEventHotKeyID), nil,
                                  MemoryLayout<EventHotKeyID>.size, nil, &pressed)
                guard pressed.signature == HotKeyCenter.signature,
                      pressed.id == HotKeyCenter.hotKeyId else { return noErr }
                // Carbon hot key events arrive on the main thread, inside the app event loop.
                MainActor.assumeIsolated {
                    let center = Unmanaged<HotKeyCenter>.fromOpaque(userData).takeUnretainedValue()
                    center.onPressed?()
                }
                return noErr
            },
            1, &event, Unmanaged.passUnretained(self).toOpaque(), &handler)
        if status != noErr {
            handler = nil
        }
    }

    /// Replaces the registered shortcut. An invalid combination unregisters everything.
    func register(_ shortcut: HotKey) {
        if let hotKey {
            UnregisterEventHotKey(hotKey)
            self.hotKey = nil
        }
        guard shortcut.isValid, handler != nil else { return }
        let id = EventHotKeyID(signature: Self.signature, id: Self.hotKeyId)
        var ref: EventHotKeyRef?
        if RegisterEventHotKey(shortcut.keyCode, shortcut.modifiers, id,
                               GetApplicationEventTarget(), 0, &ref) == noErr {
            hotKey = ref
        }
    }
}

/// macOS-specific delivery of the selected clipboard item to the original application.
@MainActor
final class ClipboardPasteDelivery {
    private var destination: NSRunningApplication?

    static var isAllowed: Bool { AXIsProcessTrusted() }

    static func requestPermission() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
        _ = AXIsProcessTrustedWithOptions(options as CFDictionary)
    }

    func rememberDestination() {
        destination = NSWorkspace.shared.frontmostApplication
    }

    func paste() {
        guard Self.isAllowed, let destination, !destination.isTerminated,
              destination.processIdentifier != ProcessInfo.processInfo.processIdentifier else { return }
        // Nonactivating panels leave the destination app active. Never deliver into another app
        // if the user switched away while the preview was open.
        let pid = destination.processIdentifier
        // Flycut's delay and event route: some apps ignore a Cmd+V sent straight to their process.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            guard NSWorkspace.shared.frontmostApplication?.processIdentifier == pid,
                  let source = CGEventSource(stateID: .combinedSessionState),
                  let down = CGEvent(keyboardEventSource: source, virtualKey: CGKeyCode(kVK_ANSI_V), keyDown: true),
                  let up = CGEvent(keyboardEventSource: source, virtualKey: CGKeyCode(kVK_ANSI_V), keyDown: false) else { return }
            // The extra bit marks the left Command key, which some apps look for.
            let command = CGEventFlags(rawValue: CGEventFlags.maskCommand.rawValue | 0x8)
            down.flags = command
            up.flags = command
            down.post(tap: .cghidEventTap)
            up.post(tap: .cghidEventTap)
        }
    }
}
