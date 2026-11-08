import AppKit
import Carbon.HIToolbox
import Observation
import ServiceManagement
import SwiftUI

/// A metric the notch can show. The declaration order is the order on screen.
enum Metric: String, CaseIterable, Identifiable {
    case cpu, memory, disk, gpu

    var id: Self { self }

    var title: String {
        switch self {
        case .cpu: "CPU"
        case .memory: "Memory"
        case .disk: "Disk"
        case .gpu: "GPU"
        }
    }

    var symbol: String {
        switch self {
        case .cpu: "cpu"
        case .memory: "memorychip"
        case .disk: "internaldrive"
        case .gpu: "display"
        }
    }

    func percent(in state: SystemState) -> Double? {
        switch self {
        case .cpu: state.cpuPercent
        case .memory: state.memoryPercent
        case .disk: state.diskPercent
        case .gpu: state.gpuPercent
        }
    }
}

/// User preferences, saved between launches.
@MainActor @Observable
final class Settings {
    static let intervals: [Double] = [0.5, 1, 2, 5, 10]
    static let clipboardCapacities: [Int] = [10, 25, 50, 100, 200]

    @ObservationIgnored var onIntervalChange: ((Double) -> Void)?
    @ObservationIgnored var onWingMetricsChange: (() -> Void)?
    @ObservationIgnored var onClipboardCapacityChange: ((Int) -> Void)?
    @ObservationIgnored var onShortcutChange: ((HotKey) -> Void)?
    @ObservationIgnored var onKeepClipboardHistoryChange: (() -> Void)?

    var interval: Double {
        didSet {
            UserDefaults.standard.set(interval, forKey: "interval")
            onIntervalChange?(interval)
        }
    }

    /// The metrics beside the collapsed notch.
    var wingMetrics: Set<Metric> {
        didSet {
            UserDefaults.standard.set(wingMetrics.map(\.rawValue), forKey: "wingMetrics")
            onWingMetricsChange?()
        }
    }

    /// How many copies the clipboard history keeps.
    var clipboardCapacity: Int {
        didSet {
            UserDefaults.standard.set(clipboardCapacity, forKey: "clipboardCapacity")
            onClipboardCapacityChange?(clipboardCapacity)
        }
    }

    /// Whether the clipboard history survives quitting the app.
    var keepClipboardHistory: Bool {
        didSet {
            UserDefaults.standard.set(keepClipboardHistory, forKey: "keepClipboardHistory")
            onKeepClipboardHistoryChange?()
        }
    }

    /// Where the history is saved, or nil when it should stay in memory.
    var clipboardHistoryFile: URL? {
        guard keepClipboardHistory else { return nil }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("HuginMunin/clipboard-history.bin")
    }

    /// The shortcut that opens the clipboard history.
    var shortcut: HotKey {
        didSet {
            let defaults = UserDefaults.standard
            defaults.set(Int(shortcut.keyCode), forKey: "shortcutKeyCode")
            defaults.set(Int(shortcut.modifiers), forKey: "shortcutModifiers")
            onShortcutChange?(shortcut)
        }
    }

    /// The chosen metrics in screen order, with the first half left of the notch.
    private var orderedWingMetrics: [Metric] { Metric.allCases.filter(wingMetrics.contains) }
    private var leftCount: Int { (orderedWingMetrics.count + 1) / 2 }
    var leftMetrics: [Metric] { Array(orderedWingMetrics.prefix(leftCount)) }
    var rightMetrics: [Metric] { Array(orderedWingMetrics.dropFirst(leftCount)) }

    init() {
        let defaults = UserDefaults.standard
        let saved = defaults.double(forKey: "interval")
        interval = Self.intervals.contains(saved) ? saved : 1
        wingMetrics = defaults.stringArray(forKey: "wingMetrics")
            .map { Set($0.compactMap(Metric.init(rawValue:))) } ?? Set(Metric.allCases)
        let savedCapacity = defaults.integer(forKey: "clipboardCapacity")
        clipboardCapacity = Self.clipboardCapacities.contains(savedCapacity) ? savedCapacity : 50
        keepClipboardHistory = defaults.object(forKey: "keepClipboardHistory") as? Bool ?? true
        if defaults.object(forKey: "shortcutKeyCode") == nil {
            shortcut = .standard
        } else {
            let saved = HotKey(keyCode: UInt32(defaults.integer(forKey: "shortcutKeyCode")),
                               modifiers: UInt32(defaults.integer(forKey: "shortcutModifiers")))
            // Migrate the former default while preserving custom combinations.
            let oldDefault = HotKey(keyCode: UInt32(kVK_ANSI_C), modifiers: UInt32(cmdKey | shiftKey))
            shortcut = saved.isValid && saved != oldDefault ? saved : .standard
        }
    }
}

/// The app's login item. macOS owns this state, because the user can also change it in System
/// Settings, so it is read from the system rather than saved with the other settings.
@MainActor @Observable
final class LoginItem {
    private(set) var status = SMAppService.mainApp.status
    private(set) var errorMessage: String?

    var isOn: Bool { status == .enabled || status == .requiresApproval }

    func refresh() { status = SMAppService.mainApp.status }

    func set(_ on: Bool) {
        do {
            if on {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
        refresh()
    }
}

struct SettingsView: View {
    @Bindable var settings: Settings
    let clipboard: ClipboardStore
    let loginItem: LoginItem
    let shortcutRecorder: ShortcutRecorder

    var body: some View {
        Form {
            Picker("Update every", selection: $settings.interval) {
                ForEach(Settings.intervals, id: \.self) { interval in
                    Text(interval.formatted() + " s").tag(interval)
                }
            }
            LabeledContent("Beside the notch") {
                HStack(spacing: 12) {
                    ForEach(Metric.allCases) { metric in
                        Toggle(metric.title, isOn: binding(for: metric))
                            .toggleStyle(.checkbox)
                    }
                }
            }
            Toggle("Launch at login", isOn: Binding(get: { loginItem.isOn }, set: { loginItem.set($0) }))
            if loginItem.status == .requiresApproval {
                HStack {
                    Text("Needs your approval in System Settings.")
                    Button("Open Login Items") { SMAppService.openSystemSettingsLoginItems() }
                }
                .font(.caption)
            }
            if let error = loginItem.errorMessage {
                Text(error).font(.caption).foregroundStyle(.red)
            }
            Section("Clipboard") {
                Picker("History length", selection: $settings.clipboardCapacity) {
                    ForEach(Settings.clipboardCapacities, id: \.self) { capacity in
                        Text("\(capacity)").tag(capacity)
                    }
                }
                ShortcutPicker(settings: settings, recorder: shortcutRecorder)
                Toggle("Keep history after quitting", isOn: $settings.keepClipboardHistory)
                Text("Saved on this Mac without encryption, readable only by your account.")
                    .font(.caption).foregroundStyle(.secondary)
                LabeledContent("Automatic paste") {
                    Button("Allow Accessibility…") { ClipboardPasteDelivery.requestPermission() }
                }
                Text("Without Accessibility permission, choosing an entry copies it; paste with ⌘V.")
                    .font(.caption).foregroundStyle(.secondary)
                LabeledContent("History") {
                    Button("Clear History") { clipboard.clear() }
                        .disabled(clipboard.entries.isEmpty)
                }
            }
        }
        .pickerStyle(.segmented)
        .padding(20)
        .frame(width: 440)
        .fixedSize()
    }

    private func binding(for metric: Metric) -> Binding<Bool> {
        Binding(get: { settings.wingMetrics.contains(metric) }, set: { shown in
            if shown {
                settings.wingMetrics.insert(metric)
            } else {
                settings.wingMetrics.remove(metric)
            }
        })
    }
}

/// Records a new global shortcut. A click starts listening; the next key combination that has
/// Cmd, Option, or Control in it becomes the shortcut, and Escape cancels. A class, because the
/// project keeps UI state in `@Observable` objects rather than `@State`.
@MainActor @Observable
final class ShortcutRecorder {
    private(set) var recording = false
    @ObservationIgnored private var monitor: Any?
    @ObservationIgnored var onRecord: ((HotKey) -> Void)?

    func toggle() { recording ? cancel() : begin() }

    func cancel() {
        if let monitor {
            NSEvent.removeMonitor(monitor)
            self.monitor = nil
        }
        recording = false
    }

    private func begin() {
        guard !recording else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            MainActor.assumeIsolated { self?.press(event) }
            return nil // the recorder swallows every key while it listens
        }
        recording = true
    }

    private func press(_ event: NSEvent) {
        let pressed = HotKey(keyCode: UInt32(event.keyCode),
                             modifiers: HotKey.carbonModifiers(event.modifierFlags))
        if pressed.isEscape {
            cancel()
        } else if pressed.isValid {
            onRecord?(pressed)
            cancel()
        }
    }
}

/// Records a new global shortcut: click, press a key combination with Cmd, Option, or Control in
/// it, and it takes effect immediately. Escape cancels the recording.
private struct ShortcutPicker: View {
    @Bindable var settings: Settings
    let recorder: ShortcutRecorder

    var body: some View {
        LabeledContent("Open clipboard history") {
            HStack(spacing: 8) {
                Button(recorder.recording ? "Press keys…" : settings.shortcut.label) {
                    recorder.toggle()
                }
                .frame(minWidth: 130)
                if settings.shortcut != .standard {
                    Button("Reset") { settings.shortcut = .standard }
                }
            }
        }
        .onDisappear { recorder.cancel() }
    }
}

/// Owns the settings window. The app has no Dock icon, so opening it must activate the app.
@MainActor
final class SettingsWindowController {
    private let settings: Settings
    private let clipboard: ClipboardStore
    private let loginItem = LoginItem()
    private let shortcutRecorder: ShortcutRecorder
    private var window: NSWindow?

    init(settings: Settings, clipboard: ClipboardStore) {
        self.settings = settings
        self.clipboard = clipboard
        shortcutRecorder = ShortcutRecorder()
        shortcutRecorder.onRecord = { [weak settings] shortcut in
            settings?.shortcut = shortcut
        }
    }

    func show() {
        loginItem.refresh()
        let window = self.window ?? makeWindow()
        self.window = window
        NSApp.activate()
        window.makeKeyAndOrderFront(nil)
    }

    private func makeWindow() -> NSWindow {
        let window = NSWindow(contentViewController: NSHostingController(
            rootView: SettingsView(settings: settings, clipboard: clipboard, loginItem: loginItem,
                                   shortcutRecorder: shortcutRecorder)
        ))
        window.title = "Hugin & Munin Settings"
        window.styleMask = [.titled, .closable]
        window.isReleasedWhenClosed = false
        window.center()
        return window
    }
}
