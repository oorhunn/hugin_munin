// C interface between the core and the Swift UI.
#ifndef HM_BRIDGE_H
#define HM_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main-thread-only session shelf. Returned paths remain valid until the next mutation.
typedef struct hm_file_shelf hm_file_shelf;
hm_file_shelf *hm_file_shelf_create(void);
void hm_file_shelf_destroy(hm_file_shelf *shelf);
int hm_file_shelf_add(hm_file_shelf *shelf, const char *path);
void hm_file_shelf_remove(hm_file_shelf *shelf, const char *path);
size_t hm_file_shelf_count(const hm_file_shelf *shelf);
const char *hm_file_shelf_path(const hm_file_shelf *shelf, size_t index);

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

// The copy/paste buffer. Entry kinds.
enum { HM_CLIPBOARD_TEXT = 0, HM_CLIPBOARD_IMAGE = 1 };

typedef struct hm_clipboard hm_clipboard;
typedef void (*hm_clipboard_callback)(void *context);

// One remembered copy. `text` is a null-terminated UTF-8 preview of at most 255 bytes for text
// entries; `image` holds the TIFF bytes of image entries. Both are owned by the snapshot they
// came from and are NULL when not applicable.
typedef struct {
    uint64_t id;
    int kind;
    double captured_at; // seconds since the Unix epoch
    size_t size;        // payload bytes the core keeps for this entry
    const char *text;
    const uint8_t *image;
    size_t image_size;
} hm_clipboard_entry;

// Watches the system pasteboard and keeps the last `capacity` copies, 1 to 500. Calls `callback`
// on a background thread when an external copy changes the history. Returns NULL if the watcher
// cannot start.
hm_clipboard *hm_clipboard_start(hm_clipboard_callback callback, void *context, size_t capacity);

// Stops watching and frees `clipboard`. No callback runs after this returns. Accepts NULL.
void hm_clipboard_stop(hm_clipboard *clipboard);

// Changes the capacity while running, with the same limits. The oldest entries beyond it are
// dropped. This does not call the callback.
void hm_clipboard_set_capacity(hm_clipboard *clipboard, size_t capacity);

// Deep copy of the history, newest first, in one malloc'd block. Returns NULL when empty or on
// allocation failure. Free it with hm_clipboard_free_snapshot once the caller has copied what it
// needs; the pointers inside are only valid until then.
hm_clipboard_entry *hm_clipboard_snapshot(hm_clipboard *clipboard, size_t *count);

// Frees a snapshot returned by hm_clipboard_snapshot. Accepts NULL.
void hm_clipboard_free_snapshot(hm_clipboard_entry *entries, size_t count);

// Writes the entry back to the pasteboard and moves it to the front of the history, so the user
// pastes it with Cmd+V. Returns false for an unknown id or a failed write. This does not call the
// callback.
bool hm_clipboard_restore(hm_clipboard *clipboard, uint64_t id);

// Empties the history, leaving the pasteboard alone. This does not call the callback.
void hm_clipboard_clear(hm_clipboard *clipboard);

#ifdef __cplusplus
}
#endif

#endif
