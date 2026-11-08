// Reads the live system through the platform backend.
#include "check.hpp"
#include "hm/collectors.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

int main()
{
  const auto first = hm::collect_cpu_cores();
  require(first.has_value(), "CPU counters are readable");
  require(!first->empty(), "At least one core");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto second = hm::collect_cpu_cores();
  require(second.has_value() && second->size() == first->size(),
          "Core count is stable");
  const auto average = hm::average_busy(*first, *second);
  require(average.has_value(), "Ticks pass between readings");
  require(*average >= 0 && *average <= 100, "Average is a percentage");

  const auto memory = hm::collect_memory();
  require(memory.has_value(), "Memory statistics are readable");
  require(memory->total > 0, "Installed memory is reported");
  require(memory->used > 0 &&
              memory->used == memory->app + memory->wired + memory->compressed,
          "Memory used accounting");
  require(memory->used <= memory->total,
          "Used memory fits in installed memory");

  const auto gpus = hm::collect_gpus();
  require(gpus.has_value(), "GPU statistics are readable");
  for (const auto &gpu : *gpus)
    require(!gpu.utilization ||
                (*gpu.utilization >= 0 && *gpu.utilization <= 100),
            "GPU utilization is a percentage");

  const auto disk = hm::collect_disk();
  require(disk.has_value(), "Disk capacity is readable");
  require(disk->total > 0 && disk->used <= disk->total,
          "Disk usage fits in capacity");
  return 0;
}
