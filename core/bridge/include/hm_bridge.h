// C interface between the core and the Swift UI.
#ifndef HM_BRIDGE_H
#define HM_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main-thread-only session shelf. Returned paths remain valid until the next
// mutation.
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
  // UTF-8 snapshot of registered jobs and capacity, valid only for the callback
  // duration.
  const char *coordination_json;
} hm_sample;

typedef struct hm_sampler hm_sampler;
typedef void (*hm_sample_callback)(const hm_sample *sample, void *context);

// Calls `callback` on a background thread every `interval_seconds`, limited to
// 0.25 to 10. Returns NULL if sampling cannot start.
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context,
                             double interval_seconds);

// Changes the interval while sampling, with the same limits.
void hm_sampler_set_interval(hm_sampler *sampler, double interval_seconds);

// Stops sampling and frees `sampler`. No callback runs after this returns.
// Accepts NULL.
void hm_sampler_stop(hm_sampler *sampler);
// May block on local IPC. Call off the main thread. Returns 1 on success, 0 on
// failure.
int hm_admissions_set_paused(int paused);

// The copy/paste buffer. Entry kinds.
enum
{
  HM_CLIPBOARD_TEXT = 0,
  HM_CLIPBOARD_IMAGE = 1
};

typedef struct hm_clipboard hm_clipboard;
typedef void (*hm_clipboard_callback)(void *context);

// One remembered copy. `text` is a null-terminated UTF-8 preview of at most
// 4095 bytes for text entries; `image` holds the TIFF bytes of image entries.
// Both are owned by the snapshot they came from and are NULL when not
// applicable.
typedef struct {
  uint64_t id;
  int kind;
  double captured_at; // seconds since the Unix epoch
  size_t size;        // payload bytes the core keeps for this entry
  const char *text;
  const uint8_t *image;
  size_t image_size;
} hm_clipboard_entry;

// Watches the system pasteboard and keeps the last `capacity` copies, 1 to 500.
// Calls `callback` on a background thread when an external copy changes the
// history. With a `history_file` path, the history starts from that file and is
// saved to it after each change; NULL keeps it in memory. Returns NULL if the
// watcher cannot start.
hm_clipboard *hm_clipboard_start(hm_clipboard_callback callback, void *context,
                                 size_t capacity, const char *history_file);

// Starts saving to `history_file`. NULL stops saving and deletes the file in
// use. Returns false if the change could not be made, which leaves saving as it
// was.
bool hm_clipboard_set_history_file(hm_clipboard *clipboard,
                                   const char *history_file);

// Stops watching and frees `clipboard`. No callback runs after this returns.
// Accepts NULL.
void hm_clipboard_stop(hm_clipboard *clipboard);

// Changes the capacity while running, with the same limits. The oldest entries
// beyond it are dropped. This does not call the callback.
void hm_clipboard_set_capacity(hm_clipboard *clipboard, size_t capacity);

// Deep copy of the history, newest first, in one malloc'd block. Returns NULL
// when empty or on allocation failure. Free it with hm_clipboard_free_snapshot
// once the caller has copied what it needs; the pointers inside are only valid
// until then.
hm_clipboard_entry *hm_clipboard_snapshot(hm_clipboard *clipboard,
                                          size_t *count);

// Frees a snapshot returned by hm_clipboard_snapshot. Accepts NULL.
void hm_clipboard_free_snapshot(hm_clipboard_entry *entries, size_t count);

// Writes the entry back to the pasteboard and leaves the history order alone.
// Returns false for an unknown id or a failed write. This does not call the
// callback.
bool hm_clipboard_restore(hm_clipboard *clipboard, uint64_t id);

// The keyboard preview's position in the history, where 0 is the newest entry.
// It persists between previews and returns to 0 when an external copy arrives.
// Moves stop at both ends.
typedef enum hm_clipboard_step
{
  HM_CLIPBOARD_STEP_OLDER,
  HM_CLIPBOARD_STEP_NEWER,
  HM_CLIPBOARD_STEP_OLDEST,
  HM_CLIPBOARD_STEP_NEWEST,
  HM_CLIPBOARD_STEP_TEN_OLDER,
  HM_CLIPBOARD_STEP_TEN_NEWER
} hm_clipboard_step;

size_t hm_clipboard_stack_position(const hm_clipboard *clipboard);
size_t hm_clipboard_stack_move(hm_clipboard *clipboard, hm_clipboard_step step);

// Goes straight to `position`, or stays put when the history has no such entry.
size_t hm_clipboard_stack_jump(hm_clipboard *clipboard, size_t position);

// Writes the entry at the stack position to the pasteboard. Returns false when
// there is none.
bool hm_clipboard_stack_restore(hm_clipboard *clipboard);

// Forgets the entry at the stack position, keeping the position valid.
void hm_clipboard_stack_remove(hm_clipboard *clipboard);

// Empties the history and deletes its file, leaving the pasteboard alone. This
// does not call the callback.
void hm_clipboard_clear(hm_clipboard *clipboard);

typedef struct {
  bool scanning;
  uint64_t files_seen, bytes_seen; // progress of the running scan
  int64_t last_scan;               // Unix time, zero before the first scan
  size_t scan_count, unreadable_count;
  const char *error; // empty when there is none; only valid during the callback
} hm_disk_status;

typedef struct {
  bool has_baseline;    // false when there is no earlier scan to compare with
  bool volume_compared; // false when either scan could not read the volume, so
                        // that volume_change and unexplained_change are zero
  int64_t before_time, after_time;
  uint64_t free_before, free_after, volume_total;
  int64_t volume_change, scanned_change, unexplained_change;
} hm_disk_summary;

typedef struct {
  const char *path; // valid until the report is freed
  uint64_t before, after;
  int64_t parent; // index of the parent row, or -1
  bool is_file, is_project;
  bool compared; // false where the earlier scan did not cover this path
} hm_disk_row;

typedef struct hm_disk_tracker hm_disk_tracker;
typedef struct hm_disk_report hm_disk_report;
typedef void (*hm_disk_status_callback)(const hm_disk_status *status,
                                        void *context);

// The folders to scan until the user chooses others. The strings live as long
// as the program.
size_t hm_disk_default_root_count(void);
const char *hm_disk_default_root(size_t index);

// Calls `callback` on a background thread straight away and whenever the status
// changes. Scans repeat every 6 hours, but only once the first has been asked
// for. Returns NULL if tracking cannot start.
hm_disk_tracker *hm_disk_tracker_start(hm_disk_status_callback callback,
                                       void *context, const char *const *roots,
                                       size_t root_count);

// Takes effect from the next scan.
void hm_disk_tracker_set_roots(hm_disk_tracker *tracker,
                               const char *const *roots, size_t root_count);

// Starts a scan unless one is running.
void hm_disk_tracker_scan(hm_disk_tracker *tracker);

// Stops any running scan and frees `tracker`. No callback runs after this
// returns. Accepts NULL.
void hm_disk_tracker_stop(hm_disk_tracker *tracker);

// Compares the latest scan with the one `since_seconds` before it, or with the
// previous scan when that is zero. Reads from disk, so call it off the main
// thread, but not at the same time as hm_disk_tracker_stop. Returns NULL when
// there is no scan to report on or one cannot be read.
hm_disk_report *hm_disk_report_create(hm_disk_tracker *tracker,
                                      int64_t since_seconds);
hm_disk_summary hm_disk_report_summary(const hm_disk_report *report);
size_t hm_disk_report_row_count(const hm_disk_report *report);
hm_disk_row hm_disk_report_row(const hm_disk_report *report, size_t index);
// Row indexes of the folders and projects that grew, largest first.
size_t hm_disk_report_hotspot_count(const hm_disk_report *report);
size_t hm_disk_report_hotspot(const hm_disk_report *report, size_t index);
// Paths left out of the report, because a scan could not read them.
size_t hm_disk_report_unreadable_count(const hm_disk_report *report);
const char *hm_disk_report_unreadable(const hm_disk_report *report,
                                      size_t index);
// Accepts NULL.
void hm_disk_report_free(hm_disk_report *report);

#ifdef __cplusplus
}
#endif

#endif
