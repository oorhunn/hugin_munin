// Copy/paste buffer: watches the system pasteboard and keeps a bounded history of what was
// copied, so an entry can be restored to the clipboard on demand.
#pragma once

#include "hm/pasteboard.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace hm {

// Default and limits for the number of remembered entries. The capacity is a user setting.
inline constexpr std::size_t clipboard_default_capacity = 50;
inline constexpr std::size_t clipboard_min_capacity = 1;
inline constexpr std::size_t clipboard_max_capacity = 500;
// Total bytes of payload the history may hold, whatever the capacity. Oldest entries are evicted
// first; a single item larger than this is not recorded at all. Not a user setting.
inline constexpr std::size_t clipboard_max_total_bytes = 64u * 1024 * 1024;
// How often the watcher polls the pasteboard change counter.
inline constexpr std::chrono::duration<double> clipboard_poll_interval{0.5};
// At most this many bytes of an entry's text cross the bridge, cut at a UTF-8 boundary.
inline constexpr std::size_t clipboard_preview_bytes = 255;

// One remembered copy. The history owns the payload; the bridge hands out previews.
struct ClipboardEntry {
    std::uint64_t id = 0; // unique, never reused
    ClipboardContent content;
    std::chrono::system_clock::time_point captured_at{};
};

// Limits a capacity chosen in Settings to a sane range. NaN-like input cannot arrive as a size_t.
std::size_t clamp_clipboard_capacity(std::size_t capacity);

// A bounded, deduplicating history of clipboard content. The newest entry is first.
//
// Deduplication: content that matches the newest entry is ignored, and content that matches an
// older entry moves that entry to the front, so the history holds each distinct payload once and
// in most-recently-used order. Memory is bounded twice over: by the entry count and by total
// payload bytes, evicting the oldest entries when a limit is exceeded.
class ClipboardHistory {
  public:
    explicit ClipboardHistory(std::size_t capacity = clipboard_default_capacity,
                              std::size_t max_total_bytes = clipboard_max_total_bytes);

    void set_capacity(std::size_t capacity);
    std::size_t capacity() const { return capacity_; }

    // Records `content` captured at `now`. Returns the entry's id when the history changed (a new
    // entry, or an existing one moved to the front), and empty when the content was a duplicate
    // of the newest entry, empty, or larger than the byte budget.
    std::optional<std::uint64_t> capture(ClipboardContent content,
                                         std::chrono::system_clock::time_point now);

    // Moves an existing entry to the front, as restoring it does. Returns false for an unknown id.
    bool promote(std::uint64_t id, std::chrono::system_clock::time_point now);

    const std::vector<ClipboardEntry> &entries() const { return entries_; }
    const ClipboardEntry *find(std::uint64_t id) const;
    std::optional<ClipboardContent> content(std::uint64_t id) const;

    void clear();

    std::size_t total_bytes() const { return total_bytes_; }

  private:
    void evict_while_over_limits();

    std::vector<ClipboardEntry> entries_; // newest first
    std::size_t capacity_;
    std::size_t max_total_bytes_;
    std::size_t total_bytes_ = 0;
    std::uint64_t next_id_ = 1;
};

// The pasteboard as injectable functions, so tests can drive the logic with fakes. Same idea as
// `Sources` in sampler.hpp.
struct PasteboardSources {
    std::function<std::uint64_t()> change_count = pasteboard_change_count;
    std::function<Collected<ClipboardContent>()> read = read_pasteboard;
    std::function<Collected<void>(const ClipboardContent &)> write = write_pasteboard;
};

// One polling pass over the pasteboard at a time. `poll` reads the change counter, and only when
// it moved does it read the pasteboard and record the content. The first pass seeds the history
// with whatever is already on the pasteboard.
class ClipboardTracker {
  public:
    explicit ClipboardTracker(ClipboardHistory &history, PasteboardSources sources = {});

    // Returns true when the history changed.
    bool poll();

    // Treats `count` as already seen, which the watcher does after its own writes so a restore
    // is not recorded as a new copy.
    void mark_seen(std::uint64_t count);

  private:
    ClipboardHistory &history_;
    PasteboardSources sources_;
    std::uint64_t last_seen_ = 0;
    // A change count whose read came back empty or failed once, and gets one retry.
    std::optional<std::uint64_t> retry_;
};

// Calls `on_change` on a background thread whenever the history changes because of an external
// copy. Calls made by the UI itself (`set_capacity`, `restore`, `clear`) do not notify, so the
// caller can refresh in its own way on its own thread.
//
// `restore` writes an entry back to the pasteboard and moves it to the front. The user pastes it
// with Cmd+V; sending the paste keystroke to other apps would need Accessibility permission.
class ClipboardWatcher {
  public:
    ClipboardWatcher(std::function<void()> on_change,
                     std::size_t capacity = clipboard_default_capacity,
                     PasteboardSources sources = {},
                     std::chrono::duration<double> poll_interval = clipboard_poll_interval);

    void set_capacity(std::size_t capacity);

    // Deep copy of the entries, newest first, safe against concurrent polls.
    std::vector<ClipboardEntry> snapshot() const;

    bool restore(std::uint64_t id);
    void clear();

  private:
    void poll_pass();

    mutable std::mutex mutex_;
    ClipboardHistory history_;
    ClipboardTracker tracker_;
    std::function<void()> on_change_;
    std::chrono::duration<double> poll_interval_;
    PasteboardSources sources_;
    std::mutex wake_mutex_;
    std::condition_variable_any wakeup_;
    std::jthread worker_; // last, so it stops before the state above is destroyed
};

} // namespace hm
