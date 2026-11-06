import AppKit
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

    @ObservationIgnored var onIntervalChange: ((Double) -> Void)?
    @ObservationIgnored var onWingMetricsChange: (() -> Void)?

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
    let loginItem: LoginItem

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
            Toggle("Launch at login", isOn: Binding(get: { loginItem.isOn }, set: loginItem.set))
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
        }
        .pickerStyle(.segmented)
        .padding(20)
        .frame(width: 420)
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

/// Owns the settings window. The app has no Dock icon, so opening it must activate the app.
@MainActor
final class SettingsWindowController {
    private let settings: Settings
    private let loginItem = LoginItem()
    private var window: NSWindow?

    init(settings: Settings) { self.settings = settings }

    func show() {
        loginItem.refresh()
        let window = self.window ?? makeWindow()
        self.window = window
        NSApp.activate()
        window.makeKeyAndOrderFront(nil)
    }

    private func makeWindow() -> NSWindow {
        let window = NSWindow(contentViewController: NSHostingController(
            rootView: SettingsView(settings: settings, loginItem: loginItem)
        ))
        window.title = "Hugin & Munin Settings"
        window.styleMask = [.titled, .closable]
        window.isReleasedWhenClosed = false
        window.center()
        return window
    }
}
