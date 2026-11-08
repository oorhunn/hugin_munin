// Pure calculations; these need no platform backend.
#include "check.hpp"
#include "hm/metrics.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
bool near(double a, double b)
{
  return std::abs(a - b) < 1e-9;
}
} // namespace

int main()
{
  using hm::CpuCoreTicks;

  const auto usage = hm::core_usage({0, 10, 20, 0, 70}, {0, 30, 30, 10, 130});
  require(usage.has_value(), "Usage from two readings");
  require(near(usage->busy, 40) && near(usage->user, 20) &&
              near(usage->system, 10) && near(usage->nice, 10) &&
              near(usage->idle, 60),
          "Usage percentages");

  const auto max = std::numeric_limits<std::uint32_t>::max();
  const auto wrapped = hm::core_usage({0, max - 1, 0, 0, 0}, {0, 3, 0, 0, 5});
  require(wrapped && near(wrapped->busy, 50), "Tick counter wrap");

  require(!hm::core_usage({0, 1, 1, 1, 1}, {1, 2, 2, 2, 2}),
          "Different cores do not compare");
  require(!hm::core_usage({0, 1, 1, 1, 1}, {0, 1, 1, 1, 1}),
          "No elapsed ticks");

  const std::array<CpuCoreTicks, 2> before{{{0, 0, 0, 0, 0}, {1, 0, 0, 0, 0}}};
  const std::array<CpuCoreTicks, 2> after{
      {{1, 25, 0, 0, 75}, {0, 75, 0, 0, 25}}};
  const auto average = hm::average_busy(before, after);
  require(average && near(*average, 50), "Average matches cores by id");
  const std::array<CpuCoreTicks, 1> other_core{{{7, 1, 1, 1, 1}}};
  require(!hm::average_busy(before, other_core),
          "Average needs a comparable core");

  require(!hm::busiest_gpu({}), "No GPU");
  const std::array<hm::Gpu, 3> gpus{
      {{20.0, 10}, {std::nullopt, 8}, {70.0, std::nullopt}}};
  require(hm::busiest_gpu(gpus) == 70.0, "Busiest GPU skips unknown ones");
  const std::array<hm::Gpu, 1> silent_gpu{{{std::nullopt, 8}}};
  require(!hm::busiest_gpu(silent_gpu), "A GPU without utilization is unknown");

  require(!hm::used_percent(1, 0), "Percent of zero total");
  require(near(*hm::used_percent(25, 100), 25), "Percent");

  require(!hm::disk_from_capacity(0, 0), "No capacity");
  require(!hm::disk_from_capacity(-1, 0), "Negative capacity");
  require(hm::disk_from_capacity(100, 40)->used == 60, "Used space");
  require(hm::disk_from_capacity(100, -5)->used == 100,
          "Negative free space clamps to zero");
  require(hm::disk_from_capacity(100, 500)->used == 0,
          "Purgeable overshoot clamps to total");
  return 0;
}
