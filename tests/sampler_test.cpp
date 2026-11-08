// The sampling logic, driven by fake sources; only the last check uses a real
// thread.
#include "check.hpp"
#include "hm/sampler.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct Fake {
  int disk_reads = 0;
  std::uint32_t user = 0, idle = 0;
  bool fail = false;

  hm::Sources sources()
  {
    return {[this]() -> hm::Collected<std::vector<hm::CpuCoreTicks>> {
              if (fail)
                return std::unexpected("cpu");
              return std::vector<hm::CpuCoreTicks>{{0, user, 0, 0, idle}};
            },
            [this]() -> hm::Collected<hm::Memory> {
              if (fail)
                return std::unexpected("memory");
              return hm::Memory{.total = 100, .used = 40};
            },
            [this]() -> hm::Collected<hm::Disk> {
              ++disk_reads;
              if (fail)
                return std::unexpected("disk");
              return hm::Disk{.total = 200, .used = 50};
            },
            [this]() -> hm::Collected<std::vector<hm::Gpu>> {
              if (fail)
                return std::unexpected("gpu");
              return std::vector<hm::Gpu>{{60.0, 19}};
            }};
  }
};
} // namespace

int main()
{
  Fake fake;
  hm::SampleTracker tracker(fake.sources(), 60s);
  const auto start = std::chrono::steady_clock::now();

  const auto first = tracker.update(start);
  require(!first.cpu_percent, "CPU needs two readings");
  require(first.core_count == 1 && first.memory && first.disk,
          "First pass reads everything");
  require(first.gpu_percent == 60.0 && first.gpu_core_count == 19,
          "First pass reads the GPU");

  fake.user = 30;
  fake.idle = 70;
  const auto second = tracker.update(start + 1s);
  require(second.cpu_percent && std::abs(*second.cpu_percent - 30) < 1e-9,
          "CPU from two passes");
  require(fake.disk_reads == 1, "Disk is not read before its interval");

  tracker.update(start + 59s);
  require(fake.disk_reads == 1, "Disk still not due");
  tracker.update(start + 60s);
  require(fake.disk_reads == 2, "Disk is read once its interval passes");

  fake.fail = true;
  const auto failed = tracker.update(start + 120s);
  require(fake.disk_reads == 3, "Failed disk read was attempted");
  require(!failed.cpu_percent, "A failed CPU read is unknown, not stale");
  require(!failed.gpu_percent, "A failed GPU read is unknown, not stale");
  require(failed.memory && failed.memory->used == 40,
          "A failed memory read keeps the last value");
  require(failed.disk && failed.disk->used == 50,
          "A failed disk read keeps the last value");

  fake.fail = false;
  fake.user = 40;
  fake.idle = 80;
  const auto recovered = tracker.update(start + 121s);
  require(recovered.cpu_percent && std::abs(*recovered.cpu_percent - 50) < 1e-9,
          "CPU resumes from the last good reading");

  std::atomic<int> calls = 0;
  {
    Fake threaded;
    hm::Sampler sampler([&calls](const hm::Sample &) { ++calls; },
                        threaded.sources(), {.sample = 10ms, .disk = 60s});
    for (int i = 0; i < 500 && calls < 3; ++i)
      std::this_thread::sleep_for(10ms);
    require(calls >= 3, "Sampling repeats");
  }
  const int stopped_at = calls;
  std::this_thread::sleep_for(50ms);
  require(calls == stopped_at, "Destroying the sampler stops sampling");

  // A long interval must not delay a change or shutdown.
  const auto before_stop = std::chrono::steady_clock::now();
  {
    std::atomic<int> changed_calls = 0;
    Fake idle;
    hm::Sampler sampler(
        [&changed_calls](const hm::Sample &) { ++changed_calls; },
        idle.sources(), {.sample = 1h, .disk = 1h});
    for (int i = 0; i < 300 && changed_calls < 2; ++i)
      std::this_thread::sleep_for(10ms);
    require(changed_calls == 2,
            "The second pass does not wait for a long interval");
    sampler.set_interval(10ms);
    for (int i = 0; i < 500 && changed_calls < 4; ++i)
      std::this_thread::sleep_for(10ms);
    require(changed_calls >= 4,
            "A new interval takes effect without waiting for the old one");
    sampler.set_interval(1h);
  }
  require(std::chrono::steady_clock::now() - before_stop < 5s,
          "Stopping interrupts the wait");

  using Seconds = std::chrono::duration<double>;
  require(hm::clamp_sample_interval(Seconds(0.01)) == Seconds(0.25),
          "Interval lower limit");
  require(hm::clamp_sample_interval(Seconds(60)) == Seconds(10),
          "Interval upper limit");
  require(hm::clamp_sample_interval(Seconds(2)) == Seconds(2),
          "Interval within limits");
  require(hm::clamp_sample_interval(Seconds(std::nan(""))) == Seconds(0.25),
          "NaN interval");
  return 0;
}
