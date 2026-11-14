import AppKit
import Carbon.HIToolbox
import HMBridge
import Observation
import os

@MainActor
private func require(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError(message) }
}

/// Yields to the main actor until `condition` holds. The deadline only bounds a failing run.
@MainActor
private func requireEventually(_ message: String, _ condition: () -> Bool) async {
    let deadline = ContinuousClock.now + .seconds(15)
    while !condition() {
        guard ContinuousClock.now < deadline else { fatalError(message) }
        try? await Task.sleep(for: .milliseconds(5))
    }
}

private func onWorker(_ body: @escaping @Sendable () -> Void) {
    let done = DispatchSemaphore(value: 0)
    DispatchQueue.global().async {
        precondition(!Thread.isMainThread)
        body()
        done.signal()
    }
    done.wait()
}

@MainActor
private func key(_ code: Int, _ flags: NSEvent.ModifierFlags = []) -> NSEvent {
    NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
                    windowNumber: 0, context: nil, characters: "", charactersIgnoringModifiers: "",
                    isARepeat: false, keyCode: UInt16(code))!
}

@MainActor
private final class FakeClipboard: ClipboardSelection {
    var entries = Array(0..<15)
    var position = 0
    var restores = 0
    var reloads = 0
    var restoreSucceeds = true
    func reload() { reloads += 1 }
    func move(_ step: hm_clipboard_step) {
        switch step {
        case HM_CLIPBOARD_STEP_NEWER: position -= 1
        case HM_CLIPBOARD_STEP_OLDER: position += 1
        case HM_CLIPBOARD_STEP_NEWEST: position = 0
        case HM_CLIPBOARD_STEP_OLDEST: position = entries.count - 1
        case HM_CLIPBOARD_STEP_TEN_NEWER: position -= 10
        case HM_CLIPBOARD_STEP_TEN_OLDER: position += 10
        default: fatalError("Unexpected step")
        }
        position = max(0, min(position, entries.count - 1))
    }
    func jump(to index: Int) { if entries.indices.contains(index) { position = index } }
    func restoreSelected() -> Bool { restores += 1; return restoreSucceeds && !entries.isEmpty }
    func removeSelected() {
        if entries.indices.contains(position) { entries.remove(at: position) }
        position = max(0, min(position, entries.count - 1))
    }
}

@main
private struct SwiftBehaviorTests {
    @MainActor
    static func main() async {
        let suite = "HuginMunin-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        shortcuts()
        settings(defaults)
        preview(defaults)
        await callbacks()
        await realLifetimes()
        print("Swift behavior tests passed")
    }

    @MainActor
    static func shortcuts() {
        require(HotKey.standard.label == "⇧⌘V", "Default shortcut label")
        require(HotKey(keyCode: UInt32(kVK_ANSI_A), modifiers: UInt32(cmdKey)).isValid,
                "Carbon key code zero is A and must be recordable")
        require(!HotKey(keyCode: 999, modifiers: UInt32(cmdKey)).isValid, "Reject out of range keys")
        require(!HotKey(keyCode: 9, modifiers: UInt32(shiftKey)).isValid, "Shift alone is not global")
        require(HotKey.carbonModifiers([.command, .shift, .capsLock, .numericPad]) == UInt32(cmdKey | shiftKey),
                "Ignore non-shortcut modifier flags")
        let recorder = ShortcutRecorder()
        var recorded: [HotKey] = []
        recorder.onRecord = { recorded.append($0) }
        recorder.toggle()
        recorder.press(key(kVK_ANSI_V, .shift))
        require(recorder.recording && recorded.isEmpty, "Invalid key keeps recorder listening")
        recorder.press(key(kVK_Escape))
        require(!recorder.recording && recorded.isEmpty, "Escape cancels without saving")
        recorder.toggle()
        recorder.press(key(kVK_ANSI_A, .command))
        require(!recorder.recording && recorded.count == 1, "Valid shortcut records once")
        recorder.press(key(kVK_ANSI_V, .command))
        require(recorded.count == 1, "Inactive recorder ignores stale delivery")
        recorder.toggle()
        recorder.toggle()
        require(!recorder.recording, "Toggle cancels")
    }

    @MainActor
    static func settings(_ defaults: UserDefaults) {
        let settings = Settings(defaults: defaults)
        require(settings.interval == 1 && settings.clipboardCapacity == 50, "Preference defaults")
        require(settings.keepClipboardHistory && settings.shortcut == .standard, "History and shortcut defaults")
        var events: [String] = []
        settings.onIntervalChange = { events.append("interval:\($0)") }
        settings.onWingMetricsChange = { events.append("wings") }
        settings.onClipboardCapacityChange = { events.append("capacity:\($0)") }
        settings.onShortcutChange = { events.append("shortcut:\($0.label)") }
        settings.onKeepClipboardHistoryChange = { events.append("history") }
        settings.interval = 2
        settings.wingMetrics = [.gpu, .cpu, .memory]
        settings.clipboardCapacity = 100
        settings.shortcut = HotKey(keyCode: UInt32(kVK_ANSI_A), modifiers: UInt32(cmdKey))
        settings.keepClipboardHistory = false
        require(events == ["interval:2.0", "wings", "capacity:100", "shortcut:⌘A", "history"],
                "Each setting emits its callback")
        require(settings.leftMetrics == [.cpu, .memory] && settings.rightMetrics == [.gpu], "Ordered metric split")
        require(settings.clipboardHistoryFile == nil, "Disabled persistence has no path")
        let loaded = Settings(defaults: defaults)
        require(loaded.interval == 2 && loaded.clipboardCapacity == 100 && !loaded.keepClipboardHistory,
                "Preferences persist in injected suite")
        require(loaded.shortcut == settings.shortcut && loaded.wingMetrics == settings.wingMetrics, "Shortcut and metrics round trip")
        loaded.keepClipboardHistory = true
        require(loaded.clipboardHistoryFile?.lastPathComponent == "clipboard-history.bin", "History path")
        defaults.set(-1, forKey: "shortcutKeyCode")
        defaults.set(999, forKey: "interval")
        defaults.set(3, forKey: "clipboardCapacity")
        require(Settings(defaults: defaults).shortcut == .standard, "Corrupt shortcut falls back without trapping")
        require(Settings(defaults: defaults).interval == 1 && Settings(defaults: defaults).clipboardCapacity == 50,
                "Unsupported saved choices fall back")
        defaults.set(kVK_ANSI_C, forKey: "shortcutKeyCode")
        defaults.set(cmdKey | shiftKey, forKey: "shortcutModifiers")
        require(Settings(defaults: defaults).shortcut == .standard, "Former default migrates")
    }

    @MainActor
    static func preview(_ defaults: UserDefaults) {
        let store = FakeClipboard()
        let settings = Settings(defaults: defaults)
        var pastes = 0
        var closes = 0
        let session = ClipboardPreviewSession(store: store, settings: settings, paste: { pastes += 1 })
        session.onClose = { closes += 1 }
        require(session.open(), "First shortcut opens preview")
        require(!session.open() && store.position == 1 && store.reloads == 1, "Repeated shortcut walks older")
        for code in [kVK_UpArrow, kVK_LeftArrow, kVK_ANSI_K] {
            store.position = 5; session.handleKey(key(code)); require(store.position == 4, "Newer key mapping")
        }
        for code in [kVK_DownArrow, kVK_RightArrow, kVK_ANSI_J] {
            store.position = 5; session.handleKey(key(code)); require(store.position == 6, "Older key mapping")
        }
        session.handleKey(key(kVK_End)); require(store.position == 14, "End selects oldest")
        session.handleKey(key(kVK_PageUp)); require(store.position == 4, "Page up selects ten newer")
        session.handleKey(key(kVK_PageDown)); require(store.position == 14, "Page down selects ten older")
        session.handleKey(key(kVK_Home)); require(store.position == 0, "Home selects newest")
        let digits = [kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
                      kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9, kVK_ANSI_0]
        for (index, code) in digits.enumerated() {
            session.handleKey(key(code, .shift)); require(store.position == index, "Digits use physical key codes")
        }
        session.handleScroll(1); require(store.position == 8, "Scroll newer")
        session.handleScroll(-1); require(store.position == 9, "Scroll older")
        session.handleScroll(0); require(store.position == 9, "Zero scroll ignored")
        session.handleKey(key(kVK_Delete)); require(store.entries.count == 14, "Delete forgets selection")
        session.handleKey(key(kVK_Escape)); session.commit()
        require(!session.isOpen && store.restores == 0 && pastes == 0 && closes == 1, "Escape cancels pending release")
        session.open(); session.cancel(); session.commit()
        require(pastes == 0 && store.restores == 0, "Focus loss cancellation never restores")
        session.open(); session.handleKey(key(kVK_ANSI_KeypadEnter)); session.commit()
        require(store.restores == 1 && pastes == 0, "Keypad Enter copies only once")
        let order = store.entries
        session.open(); session.handleKey(key(kVK_Return)); session.commit(); session.commit()
        require(store.restores == 2 && pastes == 1 && store.entries == order, "Return and repeated release paste exactly once without reordering")
        session.open(); session.handleKey(key(kVK_ANSI_V)); require(store.position == 10, "Shortcut key walks older")
        session.handleKey(key(kVK_ANSI_V, .shift)); require(store.position == 9, "Shift reverses shortcut key")
        settings.shortcut = HotKey(keyCode: UInt32(kVK_ANSI_B), modifiers: UInt32(optionKey))
        session.handleKey(key(kVK_ANSI_B, .option)); require(store.position == 10, "Custom shortcut key walks older")
        session.handleKey(key(kVK_ANSI_B, [.option, .shift])); require(store.position == 9, "Custom shortcut reverses with Shift")
        session.cancel()
        store.entries = []
        session.open(); session.commit()
        require(pastes == 1 && !session.isOpen, "Empty history closes without pasting")
        store.entries = [1]; store.position = 0; store.restoreSucceeds = false
        session.open(); session.commit()
        require(pastes == 1, "Failed restore does not paste stale clipboard")
        session.handleKey(key(kVK_Delete)); require(store.entries == [1], "Closed session ignores input")

        require(ClipboardPreviewSession.modifiersReleased([]), "Nothing held commits")
        require(ClipboardPreviewSession.modifiersReleased([.capsLock, .numericPad, .function]),
                "Lock and device flags do not count as held")
        for held: NSEvent.ModifierFlags in [.command, .shift, .option, .control] {
            require(!ClipboardPreviewSession.modifiersReleased(held), "A held modifier keeps the preview open")
            require(!ClipboardPreviewSession.modifiersReleased([held, .capsLock]), "Caps Lock does not hide a held modifier")
        }
    }

    @MainActor
    static func callbacks() async {
        var monitor: Monitor? = Monitor()
        weak let weakMonitor = monitor
        let address = UInt(bitPattern: Unmanaged.passUnretained(monitor!).toOpaque())
        onWorker {
            let json = strdup("{\"jobs\":[],\"capacity\":{\"pressure\":\"normal\"},\"registered_agents\":7}")!
            var sample = hm_sample()
            sample.cpu_percent = 42
            sample.memory_percent = .nan
            sample.coordination_json = UnsafePointer(json)
            receiveSample(&sample, UnsafeMutableRawPointer(bitPattern: address))
            // C owns this memory only until the callback returns.
            memset(json, 120, strlen(json))
            free(json)
        }
        require(monitor!.state.cpuPercent == nil, "Worker callback queues main actor publication")
        await requireEventually("Sample reaches the main actor") { monitor!.state.cpuPercent == 42 }
        require(monitor!.state.registeredAgents == 7 &&
                monitor!.state.pressure == "normal", "Callback deep copies borrowed sample and JSON")
        onWorker {
            var sample = hm_sample()
            receiveSample(&sample, UnsafeMutableRawPointer(bitPattern: address))
        }
        monitor!.stop()
        monitor = nil
        require(weakMonitor != nil, "Queued publication retains receiver after stop")
        await requireEventually("Sample callback releases receiver") { weakMonitor == nil }

        let live = Monitor()
        let liveAddress = UInt(bitPattern: Unmanaged.passUnretained(live).toOpaque())
        onWorker {
            var sample = hm_sample()
            sample.cpu_percent = 23
            sample.memory_percent = .nan
            receiveSample(&sample, UnsafeMutableRawPointer(bitPattern: liveAddress))
        }
        await requireEventually("Sample publishes on main actor") { live.state.cpuPercent == 23 }
        require(live.state.memoryPercent == nil, "Unknown values map to nil")
        var clipboard: ClipboardStore? = ClipboardStore()
        weak let weakClipboard = clipboard
        let clipboardAddress = UInt(bitPattern: Unmanaged.passUnretained(clipboard!).toOpaque())
        onWorker { receiveClipboardChange(UnsafeMutableRawPointer(bitPattern: clipboardAddress)) }
        clipboard!.stop(); clipboard!.stop()
        clipboard = nil
        require(weakClipboard != nil, "Queued clipboard callback retains receiver through stop")
        await requireEventually("Clipboard callback releases receiver after queued reload") { weakClipboard == nil }
        receiveSample(nil, nil)
        receiveClipboardChange(nil)
    }

    /// Runs the real sampler and clipboard watcher, whose threads hold their receiver unretained
    /// until `stop` returns. The watcher only reads the system clipboard, and nothing is saved.
    @MainActor
    static func realLifetimes() async {
        var monitor: Monitor? = Monitor()
        weak let weakMonitor = monitor
        monitor!.start(interval: 0.5)
        monitor!.start(interval: 0.5) // A second sampler would outlive stop and call a freed monitor.
        await requireEventually("The core's sampler thread publishes a sample") { monitor!.state.coreCount > 0 }
        monitor!.stop()
        monitor!.stop()
        // Let anything queued before stop land, so only a later sample can count as a change.
        try? await Task.sleep(for: .milliseconds(100))
        let changed = OSAllocatedUnfairLock(initialState: false)
        withObservationTracking({ _ = monitor!.state }, onChange: { changed.withLock { $0 = true } })
        try? await Task.sleep(for: .seconds(1.5))
        require(!changed.withLock { $0 }, "No sample is published after stop returns")
        monitor = nil
        await requireEventually("A stopped monitor is released") { weakMonitor == nil }

        var clipboard: ClipboardStore? = ClipboardStore()
        weak let weakClipboard = clipboard
        clipboard!.start(capacity: 5, historyFile: nil)
        clipboard!.start(capacity: 5, historyFile: nil)
        clipboard!.stop()
        clipboard!.stop()
        let count = clipboard!.entries.count
        clipboard!.reload()
        clipboard!.move(HM_CLIPBOARD_STEP_OLDER)
        clipboard!.jump(to: 3)
        require(clipboard!.entries.count == count && clipboard!.position == 0, "A stopped store ignores calls")
        clipboard = nil
        await requireEventually("A stopped store is released") { weakClipboard == nil }
    }
}
