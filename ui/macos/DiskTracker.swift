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

/// Owns the core's tracker. Reports are built off the main thread, so the lock keeps the tracker
/// from being stopped under one.
private final class TrackerHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var tracker: OpaquePointer?

    init(_ tracker: OpaquePointer?) { self.tracker = tracker }

    func report(since seconds: Int64) -> DiskReportData? {
        lock.withLock {
            guard let report = hm_disk_report_create(tracker, seconds) else { return nil }
            defer { hm_disk_report_free(report) }
            return DiskReportData(report)
        }
    }

    func use(_ body: (OpaquePointer?) -> Void) { lock.withLock { body(tracker) } }

    func stop() {
        lock.withLock {
            hm_disk_tracker_stop(tracker)
            tracker = nil
        }
    }
}

/// Runs on the tracker's thread, so it must stay outside the main actor. See `receiveSample`.
private func receiveDiskStatus(_ status: UnsafePointer<hm_disk_status>?,
                               _ context: UnsafeMutableRawPointer?) {
    guard let status, let context else { return }
    let copy = DiskScanStatus(status.pointee)
    let tracker = Unmanaged<DiskTracker>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor in tracker.update(copy) }
}

/// Publishes the core's disk tracking on the main actor.
@MainActor @Observable
final class DiskTracker {
    private(set) var status = DiskScanStatus()
    private(set) var report: DiskReportData?

    var period = DiskPeriod.week {
        didSet { reload() }
    }

    /// The folders to scan, saved between launches.
    var roots: [String] {
        didSet {
            UserDefaults.standard.set(roots, forKey: "diskRoots")
            withRoots { roots, count in
                handle?.use { hm_disk_tracker_set_roots($0, roots, count) }
            }
        }
    }

    @ObservationIgnored private var handle: TrackerHandle?
    @ObservationIgnored private var reportTask: Task<Void, Never>?

    init() {
        roots = UserDefaults.standard.stringArray(forKey: "diskRoots") ?? Self.defaultRoots
    }

    static var defaultRoots: [String] {
        (0..<hm_disk_default_root_count()).map { String(cString: hm_disk_default_root($0)) }
    }

    /// The tracker must stay alive until `stop` returns, since the core holds it unretained.
    func start() {
        guard handle == nil else { return }
        let context = Unmanaged.passUnretained(self).toOpaque()
        withRoots { roots, count in
            handle = TrackerHandle(hm_disk_tracker_start(receiveDiskStatus, context, roots, count))
        }
    }

    func scan() {
        handle?.use { hm_disk_tracker_scan($0) }
    }

    func stop() {
        reportTask?.cancel()
        handle?.stop()
        handle = nil
    }

    fileprivate func update(_ new: DiskScanStatus) {
        // A scan that ends in the same second as the last one replaces it, so neither the time
        // nor the count has to change.
        let finished = (status.scanning && !new.scanning)
            || new.lastScan != status.lastScan || new.scanCount != status.scanCount
        status = new
        if finished || (report == nil && new.scanCount > 0) {
            reload()
        }
    }

    private func reload() {
        guard let handle else { return }
        let seconds = period.rawValue
        reportTask?.cancel()
        reportTask = Task { [weak self] in
            let report = await Task.detached(priority: .utility) {
                handle.report(since: seconds)
            }.value
            guard !Task.isCancelled else { return }
            self?.report = report
        }
    }

    /// Passes `roots` to the core as a C array, valid only during `body`.
    private func withRoots(_ body: (UnsafePointer<UnsafePointer<CChar>?>?, Int) -> Void) {
        let copies = roots.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers = copies.map { UnsafePointer<CChar>($0) }
        pointers.withUnsafeBufferPointer { body($0.baseAddress, $0.count) }
    }
}
