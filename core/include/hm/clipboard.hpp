// Copy/paste buffer: watches the system pasteboard and keeps a bounded history
// of what was copied, so an entry can be restored to the clipboard on demand.
#pragma once

#include "hm/pasteboard.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace hm {

// Default and limits for the number of remembered entries. The capacity is a
// user setting.
inline constexpr std::size_t clipboard_default_capacity = 50;
inline constexpr std::size_t clipboard_min_capacity = 1;
inline constexpr std::size_t clipboard_max_capacity = 500;
// Total bytes of payload the history may hold, whatever the capacity. Oldest
// entries are evicted first; a single item larger than this is not recorded at
// all. Not a user setting.
inline constexpr std::size_t clipboard_max_total_bytes =
    std::size_t{64} * 1024 * 1024;
// How often the watcher polls the pasteboard change counter.
inline constexpr std::chrono::duration<double> clipboard_poll_interval{0.5};
// At most this many bytes of an entry's text cross the bridge, cut at a UTF-8
// boundary.
inline constexpr std::size_t clipboard_preview_bytes = 4095;

// One remembered copy. The history owns the payload; the bridge hands out
// previews.
struct ClipboardEntry {
  std::uint64_t id = 0; // unique, never reused
  ClipboardContent content;
  std::chrono::system_clock::time_point captured_at;
};

// Limits a capacity chosen in Settings to a sane range. NaN-like input cannot
// arrive as a size_t.
std::size_t clamp_clipboard_capacity(std::size_t capacity);

// A move through the history while browsing it from the keyboard.
enum class ClipboardStep
{
  older,
  newer,
  oldest,
  newest,
  ten_older,
  ten_newer
};

// Where `step` lands from `position` in a history of `count` entries. Position
// 0 is the newest entry, and movement stops at both ends.
std::size_t clipboard_stack_step(std::size_t position, std::size_t count,
                                 ClipboardStep step);

// A bounded, deduplicating history of clipboard content. The newest entry is
// first.
//
// Content that matches the newest entry is ignored. Content that matches an
// older entry is a new entry, as in Flycut's default settings, so the history
// is a plain record of what was copied. Memory is bounded twice over: by the
// entry count and by total payload bytes, evicting the oldest entries when a
// limit is exceeded.
class ClipboardHistory {
public:
  explicit ClipboardHistory(
      std::size_t capacity = clipboard_default_capacity,
      std::size_t max_total_bytes = clipboard_max_total_bytes);

  void set_capacity(std::size_t capacity);
  [[nodiscard]] std::size_t capacity() const
  {
    return capacity_;
  }

  // Records `content` captured at `now`. Returns the new entry's id, and empty
  // when the content was a duplicate of the newest entry, empty, or larger than
  // the byte budget.
  std::optional<std::uint64_t>
  capture(ClipboardContent content, std::chrono::system_clock::time_point now);

  // Replaces the history with `saved`, newest first, giving the entries fresh
  // ids. Entries beyond the limits are dropped, oldest first.
  void adopt(std::vector<ClipboardEntry> saved);

  // Forgets one entry. Returns false for an unknown id.
  bool remove(std::uint64_t id);

  [[nodiscard]] const std::vector<ClipboardEntry> &entries() const
  {
    return entries_;
  }
  [[nodiscard]] const ClipboardEntry *find(std::uint64_t id) const;
  [[nodiscard]] std::optional<ClipboardContent> content(std::uint64_t id) const;

  void clear();

  [[nodiscard]] std::size_t total_bytes() const
  {
    return total_bytes_;
  }

private:
  void evict_while_over_limits();

  std::vector<ClipboardEntry> entries_; // newest first
  std::size_t capacity_;
  std::size_t max_total_bytes_;
  std::size_t total_bytes_ = 0;
  std::uint64_t next_id_ = 1;
};

// Writes `entries` to `file`, readable only by the owner, replacing it
// atomically. The content is not encrypted.
Collected<void>
save_clipboard_history(const std::filesystem::path &file,
                       const std::vector<ClipboardEntry> &entries);

// Reads a file written by save_clipboard_history. A missing or damaged file is
// an error.
Collected<std::vector<ClipboardEntry>>
load_clipboard_history(const std::filesystem::path &file);

// The pasteboard as injectable functions, so tests can drive the logic with
// fakes. Same idea as `Sources` in sampler.hpp.
struct PasteboardSources {
  std::function<std::uint64_t()> change_count = pasteboard_change_count;
  std::function<Collected<ClipboardContent>()> read = read_pasteboard;
  std::function<Collected<void>(const ClipboardContent &)> write =
      write_pasteboard;
};

// One polling pass over the pasteboard at a time. `poll` reads the change
// counter, and only when it moved does it read the pasteboard and record the
// content. The first pass seeds the history with whatever is already on the
// pasteboard.
class ClipboardTracker {
public:
  explicit ClipboardTracker(ClipboardHistory &history,
                            PasteboardSources sources = {});

  // Returns true when the history changed.
  bool poll();

  // Treats `count` as already seen, which the watcher does after its own writes
  // so a restore is not recorded as a new copy.
  void mark_seen(std::uint64_t count);

private:
  ClipboardHistory &history_;
  PasteboardSources sources_;
  std::uint64_t last_seen_ = 0;
  // A change count whose read came back empty or failed once, and gets one
  // retry.
  std::optional<std::uint64_t> retry_;
};

// Calls `on_change` on a background thread whenever the history changes because
// of an external copy. Calls made by the UI itself (`set_capacity`, `restore`,
// `clear`) do not notify, so the caller can refresh in its own way on its own
// thread.
//
// `restore` writes an entry back to the pasteboard and leaves the history order
// alone. Sending the paste keystroke is the UI's job, because it is
// platform-specific.
//
// The stack position is the entry the keyboard preview shows. It survives
// between previews, so consecutive pastes can walk the history, and an external
// copy resets it to the newest entry.
//
// With a history file, the watcher starts from what the file holds and saves
// after each change on its own thread, and once more when it stops. `clear`
// deletes the file straight away.
class ClipboardWatcher {
public:
  explicit ClipboardWatcher(
      std::function<void()> on_change,
      std::size_t capacity = clipboard_default_capacity,
      PasteboardSources sources = {},
      std::chrono::duration<double> poll_interval = clipboard_poll_interval,
      const std::filesystem::path &history_file = {});

  void set_capacity(std::size_t capacity);

  // Starts saving to `file`. An empty path stops saving and deletes the file in
  // use.
  void set_history_file(std::filesystem::path file);

  // Deep copy of the entries, newest first, safe against concurrent polls.
  std::vector<ClipboardEntry> snapshot() const;

  bool restore(std::uint64_t id);
  void clear();

  [[nodiscard]] std::size_t stack_position() const;
  std::size_t stack_move(ClipboardStep step);
  // Goes straight to `position`, or stays put when the history has no such
  // entry.
  std::size_t stack_jump(std::size_t position);
  bool stack_restore();
  void stack_remove();

private:
  void poll_pass();
  void keep_stack_position_in_bounds();
  void save_if_dirty();
  bool write_back(std::uint64_t id); // needs mutex_ held

  mutable std::mutex mutex_;
  ClipboardHistory history_;
  ClipboardTracker tracker_;
  std::size_t stack_position_ = 0;
  bool dirty_ = false;    // guarded by mutex_
  std::mutex save_mutex_; // taken before mutex_, held across file access
  std::filesystem::path history_file_; // guarded by save_mutex_
  std::function<void()> on_change_;
  std::chrono::duration<double> poll_interval_;
  PasteboardSources sources_;
  std::mutex wake_mutex_;
  std::condition_variable_any wakeup_;
  std::jthread worker_; // last, so it stops before the state above is destroyed
};

} // namespace hm
