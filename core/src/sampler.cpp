#include "hm/sampler.hpp"

#include <algorithm>
#include <stop_token>
#include <utility>

namespace hm {

SampleTracker::SampleTracker(Sources sources, std::chrono::duration<double> disk_interval)
    : sources_(std::move(sources)), disk_interval_(disk_interval) {}

const Sample &SampleTracker::update(std::chrono::steady_clock::time_point now) {
    if (auto cores = sources_.cpu_cores()) {
        sample_.cpu_percent = average_busy(previous_cores_, *cores);
        sample_.core_count = cores->size();
        previous_cores_ = std::move(*cores);
    } else {
        sample_.cpu_percent.reset();
    }
    const auto pressure = sources_.memory_pressure();
    sample_.memory_pressure = pressure ? std::optional(*pressure) : std::nullopt;
    const auto memory = sources_.memory();
    sample_.memory_fresh = memory.has_value();
    if (memory)
        sample_.memory = *memory;
    if (const auto gpus = sources_.gpus(); gpus && !gpus->empty()) {
        sample_.gpu_percent = busiest_gpu(*gpus);
        sample_.gpu_core_count = gpus->front().core_count;
    } else {
        sample_.gpu_percent.reset();
    }
    if (!last_disk_read_ || now - *last_disk_read_ >= disk_interval_) {
        last_disk_read_ = now;
        const auto disk = sources_.disk();
        sample_.disk_fresh = disk.has_value();
        if (disk)
            sample_.disk = *disk;
    }
    return sample_;
}

std::chrono::duration<double> clamp_sample_interval(std::chrono::duration<double> interval) {
    constexpr std::chrono::duration<double> shortest{0.25};
    constexpr std::chrono::duration<double> longest{10};
    if (!(interval >= shortest))
        return shortest;
    return std::min(interval, longest);
}

Sampler::Sampler(std::function<void(const Sample &)> callback, Sources sources, Intervals intervals)
    : interval_(intervals.sample),
      worker_([this, callback = std::move(callback), sources = std::move(sources),
               disk_interval = intervals.disk](const std::stop_token &stop) {
          SampleTracker tracker(sources, disk_interval);
          // CPU usage needs two readings, so the second one should not wait for a long interval.
          bool first_pass = true;
          while (!stop.stop_requested()) {
              callback(tracker.update(std::chrono::steady_clock::now()));
              std::unique_lock lock(mutex_);
              const auto interval =
                  first_pass ? std::min(interval_, std::chrono::duration<double>(1)) : interval_;
              first_pass = false;
              wakeup_.wait_for(lock, stop, interval, [this] { return interval_changed_; });
              interval_changed_ = false;
          }
      }) {}

void Sampler::set_interval(std::chrono::duration<double> interval) {
    {
        const std::scoped_lock lock(mutex_);
        interval_ = interval;
        interval_changed_ = true;
    }
    wakeup_.notify_all();
}

} // namespace hm
