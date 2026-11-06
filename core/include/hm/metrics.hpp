// Platform-neutral metric types and calculations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hm {

// Cumulative per-core tick counters. A single reading says nothing about usage; compare two.
struct CpuCoreTicks {
    unsigned int id;
    std::uint32_t user, system, nice, idle;
};

// Percentages of the time between two readings.
struct CoreUsage {
    double busy, user, system, nice, idle;
};

// System-wide memory in bytes. On macOS, used = app + wired + compressed, as in Activity Monitor.
struct Memory {
    std::uint64_t total = 0, used = 0, app = 0, wired = 0, compressed = 0, cached = 0;
    std::uint64_t swap_used = 0, swap_total = 0;
};

// Startup volume in bytes. Purgeable space counts as free.
struct Disk {
    std::uint64_t total = 0, used = 0;
};

// Either field is empty when the driver does not report it.
struct Gpu {
    std::optional<double> utilization; // percent
    std::optional<std::size_t> core_count;
};

// Empty when the readings are for different cores or no ticks passed between them.
std::optional<CoreUsage> core_usage(const CpuCoreTicks &before, const CpuCoreTicks &after);

// Mean busy percentage of the cores present in both readings; empty when none can be compared.
std::optional<double> average_busy(std::span<const CpuCoreTicks> before,
                                   std::span<const CpuCoreTicks> after);

// Highest utilization across the GPUs; empty when none reports one.
std::optional<double> busiest_gpu(std::span<const Gpu> gpus);

// Empty when total is zero.
std::optional<double> used_percent(std::uint64_t used, std::uint64_t total);

// Builds a Disk from the signed capacities the OS reports, clamping free space to [0, total].
std::optional<Disk> disk_from_capacity(std::int64_t total, std::int64_t available);

} // namespace hm
