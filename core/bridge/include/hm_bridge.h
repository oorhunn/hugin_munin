// C interface between the core and the Swift UI.
#ifndef HM_BRIDGE_H
#define HM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Percentages are NaN and sizes are zero until the value is known.
typedef struct {
    double cpu_percent, memory_percent, disk_percent, gpu_percent;
    uint64_t memory_used, memory_total, disk_free;
    size_t core_count, gpu_core_count;
} hm_sample;

typedef struct hm_sampler hm_sampler;
typedef void (*hm_sample_callback)(const hm_sample *sample, void *context);

// Calls `callback` on a background thread every `interval_seconds`, limited to 0.25 to 10. Returns
// NULL if sampling cannot start.
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context, double interval_seconds);

// Changes the interval while sampling, with the same limits.
void hm_sampler_set_interval(hm_sampler *sampler, double interval_seconds);

// Stops sampling and frees `sampler`. No callback runs after this returns. Accepts NULL.
void hm_sampler_stop(hm_sampler *sampler);

#ifdef __cplusplus
}
#endif

#endif
