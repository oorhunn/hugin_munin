import AppKit
import Observation
import SwiftUI

/// The choices made in the window, which last while the app runs.
@MainActor @Observable
final class DiskViewOptions {
    var showFiles = false
    var order = DiskOrder.change
    var showRoots = false
}

/// Shows what has used up the disk, in a window of its own.
struct DiskView: View {
    @Bindable var tracker: DiskTracker
    @Bindable var options: DiskViewOptions

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            driveSelector
            header
            if !tracker.selectedAvailable {
                Label("Drive offline or locked. Showing the last saved scan.", systemImage: "externaldrive.badge.xmark")
                    .font(.callout).foregroundStyle(.secondary)
            }
            if !tracker.actionError.isEmpty {
                Text(tracker.actionError).foregroundStyle(.red).font(.callout)
            }
            if let report = tracker.selectedReport {
                summary(report)
                if !report.hotspots.isEmpty {
                    hotspots(report.hotspots)
                }
                details(report)
            } else {
                emptyState
            }
            footer
        }
        .environment(\.diskDriveAvailable, tracker.selectedAvailable)
        .padding(16)
        .frame(minWidth: 620, minHeight: 520)
    }

    private var driveSelector: some View {
        HStack {
            Picker("Drive", selection: $tracker.selectedVolumeID) {
                if tracker.volumes.isEmpty { Text("Internal disk").tag("") }
                ForEach(tracker.volumes) { Text($0.title).tag($0.id) }
            }
            .disabled(tracker.editing)
            Spacer()
            Button("Add Drive or Folder…", action: addFolder)
                .disabled(tracker.editing)
        }
    }

    private func addFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.message = "Choose an external drive or a folder to track. Click Scan Now to start."
        guard panel.runModal() == .OK, let path = panel.url?.path(percentEncoded: false) else { return }
        tracker.addFolder(path)
    }

    private var header: some View {
        HStack {
            Picker("Compare with", selection: $tracker.period) {
                ForEach(DiskPeriod.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 320)
            Spacer()
            Button("Folders…") { options.showRoots = true }
                .popover(isPresented: $options.showRoots, arrowEdge: .bottom) {
                    DiskRootsView(tracker: tracker)
                }
            Button(tracker.selectedVolume?.tracked == false ? "Track and Scan" : "Scan Now", action: tracker.scan)
                .disabled(tracker.selectedStatus.scanning || !tracker.selectedAvailable || tracker.editing)
        }
    }

    private func summary(_ report: DiskReportData) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(report.headline).font(.title3.weight(.semibold))
            if let detail = explanation(report) {
                Text(detail).font(.callout).foregroundStyle(.secondary)
            }
        }
    }

    private func explanation(_ report: DiskReportData) -> String? {
        guard report.baseline != nil else { return nil }
        let scanned = "Scanned folders changed by \(Format.change(report.scannedChange))."
        guard report.volumeCompared, report.unexplainedChange.magnitude >= 100 * 1024 * 1024 else {
            return scanned
        }
        return scanned + " The remaining \(Format.change(report.unexplainedChange)) happened "
            + "outside them, or is system data such as snapshots and purgeable space."
    }

    private func hotspots(_ items: [DiskItem]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("What used the space").font(.headline)
            ForEach(items.prefix(6)) { item in
                HStack(spacing: 6) {
                    DiskIcon(item: item)
                    Text(item.displayPath).lineLimit(1).truncationMode(.middle)
                    if item.isProject { ProjectTag() }
                    Spacer()
                    ChangeText(change: item.change)
                }
                .contextMenu { DiskItemMenu(item: item) }
            }
        }
    }

    private func details(_ report: DiskReportData) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Picker("Show", selection: $options.showFiles) {
                    Text("Folders").tag(false)
                    Text("Large files").tag(true)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: 200)
                Spacer()
                Picker("Sort by", selection: $options.order) {
                    ForEach(DiskOrder.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.menu)
                .fixedSize()
            }
            if options.showFiles {
                List(options.order.sorted(report.files)) { DiskItemRow(item: $0, showPath: true) }
            } else {
                List(options.order.sorted(report.tree), children: \.children) {
                    DiskItemRow(item: $0, showPath: false)
                }
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: "internaldrive").font(.largeTitle).foregroundStyle(.secondary)
            Text("No scans yet").font(.title3.weight(.semibold))
            Text("""
            A scan records the size of your folders. Each scan after that shows what grew. \
            Scans then repeat every 6 hours while the app runs. macOS may ask for access to \
            some folders.
            """)
            .font(.callout)
            .foregroundStyle(.secondary)
            .multilineTextAlignment(.center)
            .frame(maxWidth: 400)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var footer: some View {
        HStack(spacing: 8) {
            if tracker.selectedStatus.scanning {
                ProgressView().controlSize(.small)
                Text("Scanning: \(tracker.selectedStatus.filesSeen.formatted()) files, "
                    + Format.bytes(tracker.selectedStatus.bytesSeen))
            } else if !tracker.selectedStatus.error.isEmpty {
                Image(systemName: "exclamationmark.triangle").foregroundStyle(.red)
                Text(tracker.selectedStatus.error).lineLimit(2)
            } else if let lastScan = tracker.selectedStatus.lastScan {
                Text("Last scan \(lastScan.formatted(date: .abbreviated, time: .shortened))")
            }
            Spacer()
            if tracker.selectedStatus.unreadableCount > 0 {
                Text("\(tracker.selectedStatus.unreadableCount.formatted()) unreadable, left out")
                    .help(tracker.selectedReport?.unreadable.prefix(20).joined(separator: "\n") ?? "")
                Button("Allow Access…", action: openFullDiskAccess)
                    .help("Open Full Disk Access in System Settings")
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
    }

    private func openFullDiskAccess() {
        let page = "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"
        if let url = URL(string: page) { NSWorkspace.shared.open(url) }
    }
}

private struct DiskItemRow: View {
    let item: DiskItem
    let showPath: Bool

    var body: some View {
        HStack(spacing: 6) {
            DiskIcon(item: item)
            VStack(alignment: .leading, spacing: 0) {
                Text(item.isTopLevel ? item.displayPath : item.name).lineLimit(1)
                if showPath {
                    Text(item.displayPath).font(.caption2).foregroundStyle(.secondary)
                        .lineLimit(1).truncationMode(.middle)
                }
            }
            if item.isProject { ProjectTag() }
            Spacer()
            Text(Format.bytes(item.after)).foregroundStyle(.secondary).monospacedDigit()
                .frame(width: 80, alignment: .trailing)
            ChangeText(change: item.change).frame(width: 90, alignment: .trailing)
        }
        .contextMenu { DiskItemMenu(item: item) }
    }
}

private struct DiskIcon: View {
    let item: DiskItem

    var body: some View {
        Image(systemName: item.isFile ? "doc" : item.isProject ? "shippingbox" : "folder")
            .foregroundStyle(.secondary)
            .frame(width: 16)
    }
}

private struct ProjectTag: View {
    var body: some View {
        Text("Project")
            .font(.caption2)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(.quaternary, in: Capsule())
    }
}

/// Growth is red and shrinkage green, because growth is what uses up the disk.
private struct ChangeText: View {
    let change: Int64

    var body: some View {
        Text(change == 0 ? "–" : Format.change(change))
            .monospacedDigit()
            .foregroundStyle(change > 0 ? Color.red : change < 0 ? Color.green : Color.secondary)
    }
}

private struct DiskDriveAvailableKey: EnvironmentKey {
    static let defaultValue = true
}

private extension EnvironmentValues {
    var diskDriveAvailable: Bool {
        get { self[DiskDriveAvailableKey.self] }
        set { self[DiskDriveAvailableKey.self] = newValue }
    }
}

private struct DiskItemMenu: View {
    let item: DiskItem
    @Environment(\.diskDriveAvailable) private var driveAvailable

    var body: some View {
        Button("Show in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: item.path)])
        }
        .disabled(!driveAvailable)
        Button("Copy Path") {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(item.path, forType: .string)
        }
    }
}

/// Chooses the folders to scan.
private struct DiskRootsView: View {
    @Bindable var tracker: DiskTracker

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Folders to scan").font(.headline)
            ForEach(tracker.roots, id: \.self) { root in
                HStack {
                    Text((root as NSString).abbreviatingWithTildeInPath).lineLimit(1)
                    Spacer()
                    Button("Remove", systemImage: "minus.circle") {
                        tracker.removeFolder(root)
                    }
                    .labelStyle(.iconOnly)
                    .buttonStyle(.borderless)
                }
            }
            if !tracker.selectedVolumeID.isEmpty && tracker.selectedVolume?.tracked == true {
                Button("Stop Tracking Drive", action: tracker.forgetSelectedVolume)
                    .disabled(tracker.selectedStatus.scanning)
            }
            Text("Changes apply from the next scan.").font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("Add Folder…", action: addFolder)
                Button("Use Defaults", action: tracker.useDefaults)
            }
        }
        .padding(14)
        .frame(width: 360)
        .disabled(tracker.editing)
    }

    private func addFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        guard panel.runModal() == .OK, let path = panel.url?.path(percentEncoded: false) else { return }
        tracker.addFolder(path)
    }
}

/// Owns the disk window. The app has no Dock icon, so opening it must activate the app.
@MainActor
final class DiskWindowController {
    private let tracker: DiskTracker
    private let options = DiskViewOptions()
    private var window: NSWindow?

    init(tracker: DiskTracker) { self.tracker = tracker }

    func show() {
        let window = self.window ?? makeWindow()
        self.window = window
        NSApp.activate()
        window.makeKeyAndOrderFront(nil)
    }

    private func makeWindow() -> NSWindow {
        let window = NSWindow(contentViewController: NSHostingController(
            rootView: DiskView(tracker: tracker, options: options)
        ))
        window.title = "Disk Tracker"
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
        window.isReleasedWhenClosed = false
        window.setContentSize(NSSize(width: 720, height: 680))
        window.center()
        return window
    }
}
