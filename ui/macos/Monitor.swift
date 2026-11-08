import Foundation
import HMBridge
import Observation

struct RegisteredJob: Decodable, Identifiable, Sendable {
    let id: String
    let agent_name: String
    let project: String
    let title: String
    let state: String
    let remaining_seconds: Double?
    let estimated_duration_seconds: Double?
    let overdue: Bool
}

private struct CoordinationSnapshot: Decodable {
    struct Capacity: Decodable { let pressure: String; let paused: Bool? }
    let jobs: [RegisteredJob]
    let capacity: Capacity?
    let registered_agents: Int?
    let error: String?
}

/// The latest sample from the core, for the UI to display.
struct SystemState: Sendable {
    var cpuPercent: Double?
    var memoryPercent: Double?
    var diskPercent: Double?
    var gpuPercent: Double?
    var memoryUsed: UInt64 = 0
    var memoryTotal: UInt64 = 0
    var diskFree: UInt64 = 0
    var jobs: [RegisteredJob] = []
    var pressure = "Connecting"
    var admissionsPaused = false
    var registeredAgents = 0
    var coreCount = 0
    var gpuCoreCount = 0

    init() {}

    init(_ sample: hm_sample) {
        if let json = sample.coordination_json,
           let data = String(cString: json).data(using: .utf8),
           let snapshot = try? JSONDecoder().decode(CoordinationSnapshot.self, from: data) {
            admissionsPaused = snapshot.capacity?.paused ?? false
            jobs = snapshot.jobs
            pressure = snapshot.error ?? snapshot.capacity?.pressure ?? "unknown"
            registeredAgents = snapshot.registered_agents ?? 0
        }
        cpuPercent = Self.known(sample.cpu_percent)
        memoryPercent = Self.known(sample.memory_percent)
        diskPercent = Self.known(sample.disk_percent)
        gpuPercent = Self.known(sample.gpu_percent)
        memoryUsed = sample.memory_used
        memoryTotal = sample.memory_total
        diskFree = sample.disk_free
        coreCount = sample.core_count
        gpuCoreCount = sample.gpu_core_count
    }

    private static func known(_ value: Double) -> Double? { value.isNaN ? nil : value }
}

/// Runs on the core's sampling thread, so it must stay outside the main actor. A closure written
/// inside `Monitor` would inherit that isolation and trap when the core calls it.
func receiveSample(_ sample: UnsafePointer<hm_sample>?, _ context: UnsafeMutableRawPointer?) {
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

    func setAdmissionsPaused(_ paused: Bool) {
        Task.detached { _ = hm_admissions_set_paused(paused ? 1 : 0) }
    }

    func setInterval(_ interval: Double) {
        hm_sampler_set_interval(sampler, interval)
    }

    func stop() {
        hm_sampler_stop(sampler)
        sampler = nil
    }
}
