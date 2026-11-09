#include "hm/metrics.hpp"

#include <algorithm>

namespace hm {

std::optional<CoreUsage> core_usage(const CpuCoreTicks &before,
                                    const CpuCoreTicks &after)
{
  if (before.id != after.id)
    return std::nullopt;
  // Unsigned subtraction accounts for one wrap of the 32-bit tick counters.
  const std::uint32_t user = after.user - before.user;
  const std::uint32_t system = after.system - before.system;
  const std::uint32_t nice = after.nice - before.nice;
  const std::uint32_t idle = after.idle - before.idle;
  const double busy = static_cast<double>(user) + system + nice;
  const double total = busy + idle;
  if (total == 0)
    return std::nullopt;
  return CoreUsage{100.0 * busy / total, 100.0 * user / total,
                   100.0 * system / total, 100.0 * nice / total,
                   100.0 * idle / total};
}

std::optional<double> average_busy(std::span<const CpuCoreTicks> before,
                                   std::span<const CpuCoreTicks> after)
{
  double sum = 0;
  std::size_t count = 0;
  for (const auto &core : after) {
    const auto previous = std::ranges::find(before, core.id, &CpuCoreTicks::id);
    if (previous == before.end())
      continue;
    if (const auto usage = core_usage(*previous, core)) {
      sum += usage->busy;
      ++count;
    }
  }
  if (count == 0)
    return std::nullopt;
  return sum / static_cast<double>(count);
}

std::optional<double> busiest_gpu(std::span<const Gpu> gpus)
{
  std::optional<double> busiest;
  for (const auto &gpu : gpus)
    if (gpu.utilization && (!busiest || *gpu.utilization > *busiest))
      busiest = gpu.utilization;
  return busiest;
}

std::optional<double> used_percent(std::uint64_t used, std::uint64_t total)
{
  if (total == 0)
    return std::nullopt;
  return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}

std::optional<Disk> disk_from_capacity(std::int64_t total,
                                       std::int64_t available)
{
  if (total <= 0)
    return std::nullopt;
  const auto free_bytes = std::clamp<std::int64_t>(available, 0, total);
  return Disk{static_cast<std::uint64_t>(total),
              static_cast<std::uint64_t>(total - free_bytes)};
}

} // namespace hm
