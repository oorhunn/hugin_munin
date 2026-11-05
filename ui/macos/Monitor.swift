import Foundation
import HMBridge
import Observation

/// The latest sample from the core, for the UI to display.
struct SystemState: Sendable {
    var cpuPercent: Double?
    var memoryPercent: Double?
    var diskPercent: Double?
    var memoryUsed: UInt64 = 0
    var memoryTotal: UInt64 = 0
    var diskFree: UInt64 = 0
    var coreCount = 0

    init() {}

    init(_ sample: hm_sample) {
        cpuPercent = Self.known(sample.cpu_percent)
        memoryPercent = Self.known(sample.memory_percent)
        diskPercent = Self.known(sample.disk_percent)
        memoryUsed = sample.memory_used
        memoryTotal = sample.memory_total
        diskFree = sample.disk_free
        coreCount = sample.core_count
    }

    private static func known(_ value: Double) -> Double? { value.isNaN ? nil : value }
}

/// Runs on the core's sampling thread, so it must stay outside the main actor. A closure written
/// inside `Monitor` would inherit that isolation and trap when the core calls it.
private func receiveSample(_ sample: UnsafePointer<hm_sample>?, _ context: UnsafeMutableRawPointer?) {
    guard let sample, let context else { return }
    let state = SystemState(sample.pointee)
    let monitor = Unmanaged<Monitor>.fromOpaque(context).takeUnretainedValue()
    Task { @MainActor in monitor.state = state }
}

/// Publishes the core's samples on the main actor.
@MainActor @Observable
final class Monitor {
    fileprivate(set) var state = SystemState()
    @ObservationIgnored private var sampler: OpaquePointer?

    /// The monitor must stay alive until `stop` returns, since the core holds it unretained.
    func start(interval: Double) {
        guard sampler == nil else { return }
        sampler = hm_sampler_start(receiveSample, Unmanaged.passUnretained(self).toOpaque(), interval)
    }

    func setInterval(_ interval: Double) {
        hm_sampler_set_interval(sampler, interval)
    }

    func stop() {
        hm_sampler_stop(sampler)
        sampler = nil
    }
}
