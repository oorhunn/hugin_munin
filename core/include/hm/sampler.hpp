// Turns the collectors into a periodic stream of samples.
#pragma once

#include "hm/collectors.hpp"
#include "hm/metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace hm {

// The latest known system state. A failed memory or disk read keeps the
// previous value.
struct Sample {
  std::optional<double>
      cpu_percent; // empty until two CPU readings can be compared
  std::size_t core_count = 0;
  std::optional<Memory> memory;
  std::optional<Disk> disk;
  std::optional<double>
      gpu_percent; // empty when unknown; never a stale reading
  std::optional<std::size_t> gpu_core_count;
};

struct Sources {
  std::function<Collected<std::vector<CpuCoreTicks>>()> cpu_cores =
      collect_cpu_cores;
  std::function<Collected<Memory>()> memory = collect_memory;
  std::function<Collected<Disk>()> disk = collect_disk;
  std::function<Collected<std::vector<Gpu>>()> gpus = collect_gpus;
};

struct Intervals {
  std::chrono::duration<double> sample{1};
  // Disk reads cost hundreds of times more than the others and change slowly.
  std::chrono::duration<double> disk{60};
};

// One sampling pass at a time, with the clock passed in so that tests control
// it.
class SampleTracker {
public:
  SampleTracker(Sources sources, std::chrono::duration<double> disk_interval);
  const Sample &update(std::chrono::steady_clock::time_point now);

private:
  Sources sources_;
  std::chrono::duration<double> disk_interval_;
  std::vector<CpuCoreTicks> previous_cores_;
  std::optional<std::chrono::steady_clock::time_point> last_disk_read_;
  Sample sample_;
};

// Limits a user-chosen sample interval to 0.25 to 10 seconds. NaN becomes the
// shortest interval.
std::chrono::duration<double>
clamp_sample_interval(std::chrono::duration<double> interval);

// Calls `callback` on a background thread straight away, again within a second
// so that CPU usage is known, and then after every interval. Destroying the
// sampler stops the thread, so it must not be destroyed from inside `callback`.
class Sampler {
public:
  explicit Sampler(std::function<void(const Sample &)> callback,
                   Sources sources = {}, Intervals intervals = {});

  // Takes effect immediately, with one extra sample rather than a wait for the
  // old interval.
  void set_interval(std::chrono::duration<double> interval);

private:
  std::mutex mutex_;
  std::condition_variable_any wakeup_;
  std::chrono::duration<double> interval_;
  bool interval_changed_ = false;
  std::jthread worker_; // last, so it stops before the state above is destroyed
};

} // namespace hm
