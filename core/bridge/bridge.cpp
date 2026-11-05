#include "hm_bridge.h"

#include "hm/file_shelf.hpp"
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
    out.gpu_percent = sample.gpu_percent.value_or(unknown);
    out.core_count = sample.core_count;
    out.gpu_core_count = sample.gpu_core_count.value_or(0);
    if (sample.memory) {
        out.memory_used = sample.memory->used;
        out.memory_total = sample.memory->total;
        out.memory_percent =
            hm::used_percent(sample.memory->used, sample.memory->total).value_or(unknown);
    }
    if (sample.disk) {
        out.disk_free = sample.disk->total - sample.disk->used;
        out.disk_percent =
            hm::used_percent(sample.disk->used, sample.disk->total).value_or(unknown);
    }
    return out;
}
} // namespace

extern "C" {
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context, double interval_seconds) {
    try {
        hm::Intervals intervals;
        intervals.sample =
            hm::clamp_sample_interval(std::chrono::duration<double>(interval_seconds));
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

struct hm_file_shelf {
    hm::FileShelf value;
};

extern "C" {
hm_file_shelf *hm_file_shelf_create(void) {
    try {
        return new hm_file_shelf;
    } catch (...) {
        return nullptr;
    }
}
void hm_file_shelf_destroy(hm_file_shelf *shelf) { delete shelf; }
int hm_file_shelf_add(hm_file_shelf *shelf, const char *path) {
    try {
        return shelf && path && shelf->value.add(path);
    } catch (...) {
        return 0;
    }
}
void hm_file_shelf_remove(hm_file_shelf *shelf, const char *path) {
    if (shelf && path)
        shelf->value.remove(path);
}
size_t hm_file_shelf_count(const hm_file_shelf *shelf) {
    return shelf ? shelf->value.paths().size() : 0;
}
const char *hm_file_shelf_path(const hm_file_shelf *shelf, size_t index) {
    return shelf && index < shelf->value.paths().size() ? shelf->value.paths()[index].c_str()
                                                        : nullptr;
}
}
