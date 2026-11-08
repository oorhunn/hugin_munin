#include "hm/coordinator.hpp"
#include "hm_bridge.h"

#include "hm/file_shelf.hpp"
#include "hm/sampler.hpp"

#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

struct hm_sampler {
    std::mutex mutex;
    std::condition_variable_any wake;
    double interval;
    bool changed = false;
    std::jthread worker;
    hm_sampler(hm_sample_callback callback, void *context, double seconds)
        : interval(seconds), worker([this, callback, context](const std::stop_token &stop) {
              while (!stop.stop_requested()) {
                  hm_sample out{};
                  out.cpu_percent = out.memory_percent = out.disk_percent = out.gpu_percent =
                      std::numeric_limits<double>::quiet_NaN();
                  std::string snapshot;
                  try {
                      hm::ensure_coordinator();
                      auto result = hm::coordinator_request(
                          {{"tool", "get_capacity"}, {"arguments", hm::Json::object()}});
                      snapshot = result.dump();
                      auto &s = result.at("system");
                      auto percent = [&](const char *key) {
                          return s.contains(key) && s[key].is_number()
                                     ? s[key].get<double>()
                                     : std::numeric_limits<double>::quiet_NaN();
                      };
                      out.cpu_percent = percent("cpu_percent");
                      out.memory_percent = percent("memory_percent");
                      out.disk_percent = percent("disk_percent");
                      out.gpu_percent = percent("gpu_percent");
                      out.core_count = s.value("core_count", size_t{0});
                      out.gpu_core_count = s.value("gpu_core_count", size_t{0});
                      out.memory_used = s.value("memory_used", uint64_t{0});
                      out.memory_total = s.value("memory_total", uint64_t{0});
                      out.disk_free = s.value("disk_free", uint64_t{0});
                  } catch (const std::exception &) {
                      snapshot = R"({"error":"Resource service unavailable","jobs":[]})";
                  }
                  out.coordination_json = snapshot.c_str();
                  callback(&out, context);
                  std::unique_lock lock(mutex);
                  wake.wait_for(lock, stop, std::chrono::duration<double>(interval),
                                [this] { return changed; });
                  changed = false;
              }
          }) {}
};
extern "C" {
int hm_admissions_set_paused(int paused) {
    try {
        hm::coordinator_request(
            {{"tool", "set_admissions_paused"}, {"arguments", {{"paused", paused != 0}}}});
        return 1;
    } catch (...) {
        return 0;
    }
}
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context, double interval_seconds) {
    if (!callback)
        return nullptr;
    try {
        return new hm_sampler(
            callback, context,
            hm::clamp_sample_interval(std::chrono::duration<double>(interval_seconds)).count());
    } catch (...) {
        return nullptr;
    }
}
void hm_sampler_set_interval(hm_sampler *sampler, double interval_seconds) {
    if (!sampler)
        return;
    {
        const std::scoped_lock guard(sampler->mutex);
        sampler->interval =
            hm::clamp_sample_interval(std::chrono::duration<double>(interval_seconds)).count();
        sampler->changed = true;
    }
    sampler->wake.notify_all();
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
