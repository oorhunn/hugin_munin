import Foundation
import HMBridge
import Observation

/// The tracker's progress and history, for the UI to display.
struct DiskScanStatus: Sendable {
    var scanning = false
    var filesSeen: UInt64 = 0
    var bytesSeen: UInt64 = 0
    var lastScan: Date?
    var scanCount = 0
    var unreadableCount = 0
    var error = ""

    init() {}

    init(_ status: hm_disk_status) {
        scanning = status.scanning
        filesSeen = status.files_seen
        bytesSeen = status.bytes_seen
        lastScan = status.last_scan == 0 ? nil : Date(timeIntervalSince1970: TimeInterval(status.last_scan))
        scanCount = status.scan_count
        unreadableCount = status.unreadable_count
        error = status.error.map { String(cString: $0) } ?? ""
    }
}

/// One folder or file in a report. `children` is nil for a row with nothing to expand.
struct DiskItem: Identifiable, Sendable {
    let id: Int
    let path: String
    let before: UInt64
    let after: UInt64
    let isFile: Bool
    let isProject: Bool
    let compared: Bool
    let isTopLevel: Bool
    var children: [DiskItem]?

    var change: Int64 { Int64(after) - Int64(before) }
    var name: String { (path as NSString).lastPathComponent }

    /// The path with the home folder shortened to "~".
    var displayPath: String { (path as NSString).abbreviatingWithTildeInPath }
}

/// What the tree and the file list are sorted by.
enum DiskOrder: String, CaseIterable, Identifiable {
    case change = "Change", size = "Size"

    var id: Self { self }

    func sorted(_ items: [DiskItem]) -> [DiskItem] {
        items.sorted { first, second in
            switch self {
            case .change where first.change.magnitude != second.change.magnitude:
                first.change.magnitude > second.change.magnitude
            default:
                first.after > second.after
            }
        }.map { item in
            var item = item
            item.children = item.children.map(sorted)
            return item
        }
    }
}

/// How far back a report looks.
enum DiskPeriod: Int64, CaseIterable, Identifiable {
    case lastScan = 0, day = 86_400, week = 604_800, month = 2_592_000

    var id: Self { self }

    var title: String {
        switch self {
        case .lastScan: "Last scan"
        case .day: "1 day"
        case .week: "1 week"
        case .month: "1 month"
        }
    }
}

/// A copy of a report from the core, built off the main thread.
struct DiskReportData: Sendable {
    var baseline: Date?
    var scanned: Date
    var freeNow: UInt64
    var volumeCompared: Bool
    var volumeChange: Int64
    var scannedChange: Int64
    var unexplainedChange: Int64
    var tree: [DiskItem]
    var files: [DiskItem]
    var hotspots: [DiskItem]
    var unreadable: [String]

    init(_ report: OpaquePointer) {
        let summary = hm_disk_report_summary(report)
        baseline = summary.has_baseline
            ? Date(timeIntervalSince1970: TimeInterval(summary.before_time)) : nil
        scanned = Date(timeIntervalSince1970: TimeInterval(summary.after_time))
        freeNow = summary.free_after
        volumeCompared = summary.volume_compared
        volumeChange = summary.volume_change
        scannedChange = summary.scanned_change
        unexplainedChange = summary.unexplained_change

        // Parents come before their children, so walking backwards finishes each row's children
        // before the row is attached to its own parent.
        let count = hm_disk_report_row_count(report)
        var items: [DiskItem] = []
        var parents: [Int] = []
        items.reserveCapacity(count)
        for index in 0..<count {
            let row = hm_disk_report_row(report, index)
            items.append(DiskItem(id: index, path: String(cString: row.path), before: row.before,
                                  after: row.after, isFile: row.is_file, isProject: row.is_project,
                                  compared: row.compared, isTopLevel: row.parent < 0,
                                  children: nil))
            parents.append(Int(row.parent))
        }
        hotspots = (0..<hm_disk_report_hotspot_count(report)).map {
            items[hm_disk_report_hotspot(report, $0)]
        }
        files = items.filter(\.isFile)

        var tops: [DiskItem] = []
        for index in items.indices.reversed() {
            if parents[index] >= 0 {
                items[parents[index]].children = (items[parents[index]].children ?? []) + [items[index]]
            } else {
                tops.append(items[index])
            }
        }
        tree = tops
        unreadable = (0..<hm_disk_report_unreadable_count(report)).map {
            String(cString: hm_disk_report_unreadable(report, $0))
        }
    }
}

extension DiskReportData {
    /// The free space and how it changed, as shown in the window and in the notch.
    var headline: String {
        let free = "\(Format.bytes(freeNow)) free"
        guard let baseline else { return "\(free). Changes appear after the next scan." }
        let since = baseline.formatted(date: .abbreviated, time: .shortened)
        guard volumeCompared else { return "\(free), compared with \(since)" }
        if volumeChange == 0 { return "\(free), the same as \(since)" }
        let direction = volumeChange > 0 ? "down" : "up"
        return "\(free), \(direction) \(Format.bytes(volumeChange.magnitude)) since \(since)"
    }
}

/// Copied volume state. Discovery and scan policy live in C++.
struct DiskVolumeData: Decodable, Identifiable, Sendable, Equatable {
    var id: String
    var name: String
    var mountPath: String
    var tracked: Bool
    var available: Bool
    var roots: [String]
    var scanning: Bool
    var filesSeen: UInt64
    var bytesSeen: UInt64
    var lastScan: Int64
    var scanCount: Int
    var unreadableCount: Int
    var error: String

    var title: String {
        let label = id.isEmpty ? name : "\(name) (\(mountPath))"
        return available ? label : "\(label) — Offline"
    }

    var status: DiskScanStatus {
        var result = DiskScanStatus()
        result.scanning = scanning
        result.filesSeen = filesSeen
        result.bytesSeen = bytesSeen
        result.lastScan = lastScan == 0 ? nil : Date(timeIntervalSince1970: TimeInterval(lastScan))
        result.scanCount = scanCount
        result.unreadableCount = unreadableCount
        result.error = error
        return result
    }
}

private struct DiskActionResult: Decodable, Sendable {
    var id: String?
    var error: String?
}

/// Serializes UI access to the handle, including stop versus detached report reads.
private final class TrackerHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var tracker: OpaquePointer?

    init(_ tracker: OpaquePointer?) { self.tracker = tracker }

    func report(since seconds: Int64, volume: String = "") -> DiskReportData? {
        lock.withLock {
            guard let tracker,
                  let report = hm_disk_volume_report_create(tracker, volume, seconds) else { return nil }
            defer { hm_disk_report_free(report) }
            return DiskReportData(report)
        }
    }

    func volumes() -> [DiskVolumeData] {
        lock.withLock {
            guard let tracker, let text = hm_disk_volumes_json(tracker) else { return [] }
            defer { hm_disk_string_free(text) }
            return (try? JSONDecoder().decode([DiskVolumeData].self,
                                              from: Data(String(cString: text).utf8))) ?? []
        }
    }

    func action(path: String? = nil, volume: String = "", roots: [String]? = nil,
                forget: Bool = false) -> DiskActionResult {
        lock.withLock {
            guard let tracker else { return DiskActionResult(error: "Disk tracker is unavailable") }
            let text: UnsafeMutablePointer<CChar>?
            if let path {
                text = hm_disk_add_root(tracker, path)
            } else if let roots {
                let copies = roots.map { strdup($0) }
                defer { copies.forEach { free($0) } }
                let pointers = copies.map { UnsafePointer<CChar>($0) }
                text = pointers.withUnsafeBufferPointer {
                    hm_disk_set_volume_roots(tracker, volume, $0.baseAddress, $0.count)
                }
            } else if forget {
                text = hm_disk_forget_volume(tracker, volume)
            } else {
                return DiskActionResult(error: "No disk action requested")
            }
            guard let text else { return DiskActionResult(error: "Could not update drive tracking") }
            defer { hm_disk_string_free(text) }
            return (try? JSONDecoder().decode(DiskActionResult.self,
                                              from: Data(String(cString: text).utf8)))
                ?? DiskActionResult(error: "Could not read drive tracking result")
        }
    }

    func scan(volume: String) {
        lock.withLock { if let tracker { hm_disk_scan_volume(tracker, volume) } }
    }

    func stop() {
        lock.withLock {
            hm_disk_tracker_stop(tracker)
            tracker = nil
        }
    }
}

private func receiveDiskStatus(_ status: UnsafePointer<hm_disk_status>?,
                               _ context: UnsafeMutableRawPointer?) {
    guard let status, let context else { return }
    let copy = DiskScanStatus(status.pointee)
    let tracker = Unmanaged<DiskTracker>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor in tracker.update(copy) }
}

@MainActor @Observable
final class DiskTracker {
    // The notch always displays the internal disk.
    private(set) var status = DiskScanStatus()
    private(set) var report: DiskReportData?
    private(set) var volumes: [DiskVolumeData] = []
    private(set) var selectedReport: DiskReportData?
    private(set) var actionError = ""
    private(set) var editing = false

    var selectedVolumeID = "" {
        didSet {
            selectedReport = nil
            actionError = ""
            reload()
        }
    }

    var selectedVolume: DiskVolumeData? { volumes.first { $0.id == selectedVolumeID } }
    var selectedStatus: DiskScanStatus { selectedVolume?.status ?? status }
    var selectedAvailable: Bool { selectedVolume?.available ?? selectedVolumeID.isEmpty }
    var roots: [String] { selectedVolume?.roots ?? initialRoots }

    var period = DiskPeriod.week {
        didSet { reload() }
    }

    @ObservationIgnored private var initialRoots: [String]
    @ObservationIgnored private var handle: TrackerHandle?
    @ObservationIgnored private var reportTask: Task<Void, Never>?
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var actionTask: Task<Void, Never>?

    init() {
        initialRoots = UserDefaults.standard.stringArray(forKey: "diskRoots") ?? Self.defaultRoots
    }

    static var defaultRoots: [String] {
        (0..<hm_disk_default_root_count()).map { String(cString: hm_disk_default_root($0)) }
    }

    func start() {
        guard handle == nil else { return }
        let context = Unmanaged.passUnretained(self).toOpaque()
        let copies = initialRoots.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers = copies.map { UnsafePointer<CChar>($0) }
        let pointer = pointers.withUnsafeBufferPointer {
            hm_disk_tracker_start(receiveDiskStatus, context, $0.baseAddress, $0.count)
        }
        guard let pointer else {
            actionError = "Disk tracker could not start"
            return
        }
        let handle = TrackerHandle(pointer)
        self.handle = handle
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                let volumes = await Task.detached(priority: .utility) { handle.volumes() }.value
                guard !Task.isCancelled else { return }
                self?.apply(volumes)
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    func scan() {
        guard let handle else { return }
        if let selectedVolume, !selectedVolume.tracked {
            addFolder(selectedVolume.mountPath, scanAfter: true)
        } else {
            handle.scan(volume: selectedVolumeID)
        }
    }

    func addFolder(_ path: String, scanAfter: Bool = false) {
        perform({ $0.action(path: path) }, selectResult: true, scanAfter: scanAfter)
    }

    func removeFolder(_ path: String) {
        let id = selectedVolumeID
        let updated = roots.filter { $0 != path }
        perform { $0.action(volume: id, roots: updated) }
    }

    func useDefaults() {
        let id = selectedVolumeID
        let defaults = id.isEmpty ? Self.defaultRoots : selectedVolume.map { [$0.mountPath] } ?? []
        perform { $0.action(volume: id, roots: defaults) }
    }

    func forgetSelectedVolume() {
        let id = selectedVolumeID
        perform({ $0.action(volume: id, forget: true) }, resetSelection: true)
    }

    private func perform(_ operation: @escaping @Sendable (TrackerHandle) -> DiskActionResult,
                         selectResult: Bool = false, scanAfter: Bool = false,
                         resetSelection: Bool = false) {
        guard let handle, !editing else { return }
        editing = true
        actionError = ""
        actionTask = Task { [weak self] in
            let result = await Task.detached(priority: .utility) { operation(handle) }.value
            let volumes = await Task.detached(priority: .utility) { handle.volumes() }.value
            guard !Task.isCancelled, let self else { return }
            editing = false
            if let error = result.error {
                actionError = error
                return
            }
            if selectResult, let id = result.id { selectedVolumeID = id }
            if resetSelection { selectedVolumeID = "" }
            apply(volumes)
            // Preserve the existing preference for internal roots. External roots live in C++.
            if let internalVolume = volumes.first(where: { $0.id.isEmpty }) {
                initialRoots = internalVolume.roots
                UserDefaults.standard.set(initialRoots, forKey: "diskRoots")
            }
            if scanAfter, let id = result.id { handle.scan(volume: id) }
            reload()
        }
    }

    private func apply(_ new: [DiskVolumeData]) {
        let old = selectedVolume
        volumes = new
        if !selectedVolumeID.isEmpty && !new.contains(where: { $0.id == selectedVolumeID }) {
            selectedVolumeID = ""
        }
        if old?.lastScan != selectedVolume?.lastScan || old?.scanCount != selectedVolume?.scanCount
            || (old?.scanning == true && selectedVolume?.scanning == false)
            || old?.mountPath != selectedVolume?.mountPath
            || (selectedReport == nil && selectedStatus.scanCount > 0) {
            reload()
        }
    }

    func stop() {
        pollTask?.cancel()
        actionTask?.cancel()
        reportTask?.cancel()
        handle?.stop()
        handle = nil
    }

    fileprivate func update(_ new: DiskScanStatus) {
        let finished = (status.scanning && !new.scanning)
            || new.lastScan != status.lastScan || new.scanCount != status.scanCount
        status = new
        if finished || (report == nil && new.scanCount > 0) { reload() }
    }

    private func reload() {
        guard let handle else { return }
        let seconds = period.rawValue
        let id = selectedVolumeID
        reportTask?.cancel()
        reportTask = Task { [weak self] in
            let (internalReport, selected) = await Task.detached(priority: .utility) {
                let internalReport = handle.report(since: seconds)
                let selected = id.isEmpty ? internalReport : handle.report(since: seconds, volume: id)
                return (internalReport, selected)
            }.value
            guard !Task.isCancelled else { return }
            self?.report = internalReport
            self?.selectedReport = selected
        }
    }
}
