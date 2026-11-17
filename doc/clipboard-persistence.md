# Clipboard history persistence

How the clipboard history file fails, recovers, and reports it. Covers GitHub issue #14. The history
in memory is always usable; this page is about the file.

## What changed

Before, a failed save was dropped without a trace: `save_if_dirty` cleared the dirty flag before
writing, so a failed save was not retried until the next copy, and deletion errors from `clear` and
`set_history_file` were ignored. The UI could not tell a saved history from an unsaved one, or a
deleted file from one that was still on disk.

## State kept by `ClipboardWatcher`

- `revision_` (guarded by `mutex_`) counts every change the file has to catch up with: an external
  copy, `set_capacity`, `stack_remove`, `clear`, and `set_history_file`.
- `saved_revision_` (guarded by `save_mutex_`) is the revision the file holds. The history is unsaved
  when saving is on and `revision_ != saved_revision_`. A save copies the entries together with the
  revision, writes without `mutex_`, and records that revision only on success, so a change made
  during the write leaves the history unsaved. A failed save leaves it unsaved too.
- `pending_deletes_` holds files that are no longer in use but could not be deleted. A path added
  again with `set_history_file` is removed from this list, because it is being rewritten, not
  deleted.
- `active_file_stale_` means the file in use must go because `clear` emptied the history. It ends
  when the file is deleted, or a later save replaces it.
- A deletion counts as done only when both the file and its `.tmp` sibling are gone. A path that never
  existed, including one below something that is not a folder, counts as gone.

Lock order is `save_mutex_`, then `mutex_`. `status_mutex_` and `wake_mutex_` are leaves. No callback
runs under any lock: `on_change` still runs from `poll_pass` outside `mutex_`, and the persistence
code calls no callbacks.

## Retry

`ClipboardRetryPolicy` (constructor argument, last): `max_automatic_retries` (default 5),
`first_delay` (2 s), `max_delay` (60 s). After the first failure the wait doubles on each retry up to
the maximum, so the defaults retry at 2, 4, 8, 16, and 32 s. The attempt happens on the watcher's
thread on the first poll after the delay.

A retry covers every pending item at once: pending deletions first, then the unsaved history. It
runs on the same schedule, and the failure count resets when:

- everything pending succeeds,
- a change the failed save has not seen arrives (a new copy gets an immediate try),
- `set_history_file` or `clear` is called, or
- `request_persistence_retry` is called.

When the retries are used up, nothing runs on its own until one of those happens, which keeps a
permanently broken path from copying the whole history every poll. The final save when the watcher
stops ignores the schedule and tries once.

## Status

```cpp
struct ClipboardPersistenceStatus {
  bool save_failed;      // the newest changes are not in the file
  bool delete_failed;    // a file that should be gone still exists
  bool retry_scheduled;  // another automatic retry will run
  std::uint64_t generation; // changes whenever the flags do
};
```

It carries no path, error text, or content. Reading it takes a leaf mutex and never waits for file
access, so it is safe on the main thread. A failure with `retry_scheduled == false` has used its
automatic retries and needs a new change or an explicit retry.

C API (`core/bridge/include/hm_bridge.h`):

```c
typedef struct {
  bool save_failed, delete_failed, retry_scheduled;
  uint64_t generation;
} hm_clipboard_persistence;

hm_clipboard_persistence hm_clipboard_persistence_state(const hm_clipboard *); // NULL gives zeros
void hm_clipboard_retry_persistence(hm_clipboard *); // returns at once; NULL is safe
```

The existing calls keep their signatures:

- `hm_clipboard_set_history_file` returns `true` once saving has switched, even when deleting the old
  file failed. The deletion stays pending and shows as `delete_failed`. It returns `false` only when
  the switch itself failed (a null watcher or an exception). It can wait behind a running save, so
  avoid calling it during latency-sensitive work. The existing Settings switch is synchronous.
- `hm_clipboard_clear` empties the history in every case and cannot fail. A failed file deletion
  shows as `delete_failed` and is retried. It also waits on file access.
- Turning saving off with `NULL` stops all future saves even when the deletion fails, and keeps the old
  path as a pending deletion.

Settings reads the status once per second while visible and after its own actions. It shows
save and deletion failures without paths or content, says when automatic retries are scheduled,
and offers **Retry History Storage**. The callback still fires only for external copies.

## Limitations

- Retry is asynchronous: the UI sees the outcome by reading the status again after a moment.
- The status says what failed, not why. A reason would need an error code or text, and the design keeps
  those out of the API on purpose.
- The `.tmp` file is cleaned on failure and on deletion, but a crash between writing it and renaming it
  leaves it behind until the next save or deletion.
- If the old file cannot be deleted, its contents (an earlier copy of the history, unencrypted) stay on
  disk until a retry succeeds. The status reports that; the UI should say so.

## Tests

`tests/clipboard_test.cpp` (`persistence_*`) uses a fake pasteboard and a fast retry policy. Failures
are deterministic and do not depend on file permissions or running as root:

- Save failure: the history file's parent is a regular file. "Fixing" it means replacing that file with
  a folder.
- Deletion failure: the history file is a folder that still holds a file.

Covered: the failure shows and the memory history stays usable; retries are bounded; spent retries do
not run again on their own; an explicit retry recovers with no new entry; a scheduled retry recovers
without one; a new entry gets a fresh try; stopping tries once more; switching to a new file after a
failure; turning saving off after a failed save; a failed deletion is reported, saving stays off, and
the pending file is deleted by a later retry; `clear` with a failed deletion; and copies plus
`set_capacity` calls during saves leave a file equal to the final history.

`tests/bridge_test.cpp` (`clipboard_persistence`) covers the same flags across the C interface.

## Status of this change

The integrated Release app built on macOS 27.0.1; all 11 tests, including core and bridge
persistence recovery, passed. The app remained running for the ten-second launch smoke test.
C++ formatting, static analysis of the changed translation units, Swift format/lint, and
`clipboard_test` under ThreadSanitizer pass. Local LLVM tools are 23.1.5; CI pins 23.1.4. Interactive Settings failure warnings and
macOS 14 runtime behavior have not been exercised.
