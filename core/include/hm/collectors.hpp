// Platform boundary: each supported OS implements these under
// core/src/platform/<os>/.
#pragma once

#include "hm/metrics.hpp"

#include <expected>
#include <string>
#include <vector>

namespace hm {

template <typename T> using Collected = std::expected<T, std::string>;

Collected<std::vector<CpuCoreTicks>> collect_cpu_cores();
Collected<Memory> collect_memory();
Collected<MemoryPressure> collect_memory_pressure();
// Empty where the platform exposes no GPU, such as in some virtual machines.
Collected<std::vector<Gpu>> collect_gpus();

// Reads the startup volume. This is much slower than the other collectors
// (about 10 ms on macOS), so callers should sample it less often.
Collected<Disk> collect_disk();

} // namespace hm
