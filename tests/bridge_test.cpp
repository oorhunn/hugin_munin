// The C interface the Swift UI uses, against the live system.
#include "check.hpp"
#include "hm_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace {
struct Received {
    std::atomic<int> calls = 0;
    hm_sample last{};
};

void receive(const hm_sample *sample, void *context) {
    auto *received = static_cast<Received *>(context);
    received->last = *sample;
    ++received->calls;
}
} // namespace

int main() {
    hm_sampler_stop(nullptr);
    hm_sampler_set_interval(nullptr, 1);

    Received received;
    auto *sampler = hm_sampler_start(receive, &received, 10);
    require(sampler != nullptr, "Sampler starts");
    hm_sampler_set_interval(sampler, 0.25);
    for (int i = 0; i < 500 && received.calls < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    hm_sampler_stop(sampler);

    require(received.calls >= 2, "Callback repeats");
    const auto calls = received.calls.load();
    const auto &sample = received.last;
    require(std::isfinite(sample.cpu_percent), "CPU is known after two passes");
    require(sample.cpu_percent >= 0 && sample.cpu_percent <= 100, "CPU is a percentage");
    require(sample.core_count > 0, "Core count");
    require(std::isnan(sample.gpu_percent) ||
                (sample.gpu_percent >= 0 && sample.gpu_percent <= 100),
            "GPU is unknown or a percentage");
    require(sample.memory_total > 0 && sample.memory_used <= sample.memory_total, "Memory sizes");
    require(sample.memory_percent > 0 && sample.memory_percent <= 100, "Memory percent");
    require(sample.disk_percent > 0 && sample.disk_percent <= 100, "Disk percent");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    require(received.calls == calls, "No callback after stop");
    return 0;
}
