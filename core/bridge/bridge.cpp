#include "hm_bridge.h"

#include "hm/sampler.hpp"

#include <exception>
#include <limits>

struct hm_sampler {
    hm::Sampler sampler;
};

namespace {
constexpr double unknown = std::numeric_limits<double>::quiet_NaN();

hm_sample to_c(const hm::Sample &sample) {
    hm_sample out{};
    out.cpu_percent = sample.cpu_percent.value_or(unknown);
    out.memory_percent = unknown;
    out.disk_percent = unknown;
    out.core_count = sample.core_count;
    if (sample.memory) {
        out.memory_used = sample.memory->used;
        out.memory_total = sample.memory->total;
        out.memory_percent =
            hm::used_percent(sample.memory->used, sample.memory->total).value_or(unknown);
    }
    if (sample.disk) {
        out.disk_free = sample.disk->total - sample.disk->used;
        out.disk_percent = hm::used_percent(sample.disk->used, sample.disk->total).value_or(unknown);
    }
    return out;
}
} // namespace

extern "C" {
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context, double interval_seconds) {
    try {
        hm::Intervals intervals;
        intervals.sample = hm::clamp_sample_interval(std::chrono::duration<double>(interval_seconds));
        return new hm_sampler{hm::Sampler(
            [callback, context](const hm::Sample &sample) {
                const auto out = to_c(sample);
                callback(&out, context);
            },
            {}, intervals)};
    } catch (const std::exception &) {
        return nullptr;
    }
}

void hm_sampler_set_interval(hm_sampler *sampler, double interval_seconds) {
    if (sampler)
        sampler->sampler.set_interval(
            hm::clamp_sample_interval(std::chrono::duration<double>(interval_seconds)));
}

void hm_sampler_stop(hm_sampler *sampler) { delete sampler; }
}
