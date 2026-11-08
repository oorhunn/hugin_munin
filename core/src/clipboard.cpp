#include "hm/clipboard.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace hm {
namespace {

// The file is a magic number, then for each entry, newest first: kind (1 byte),
// capture time in milliseconds since the epoch (8 bytes), payload size (8
// bytes), and the payload. Integers use the machine's byte order, because the
// file never leaves the machine.
constexpr std::array<char, 8> history_magic{'H', 'M', 'C', 'L',
                                            'I', 'P', '0', '1'};

template <typename T> void put(std::ostream &out, T value)
{
  out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename T> bool get(std::istream &in, T &value)
{
  return static_cast<bool>(
      in.read(reinterpret_cast<char *>(&value), sizeof(value)));
}

ClipboardHistory opened_history(std::size_t capacity,
                                const std::filesystem::path &file)
{
  ClipboardHistory history(capacity);
  // A missing or damaged file just means there is nothing to bring back.
  if (!file.empty())
    if (auto saved = load_clipboard_history(file))
      history.adopt(std::move(*saved));
  return history;
}

} // namespace

std::size_t clamp_clipboard_capacity(std::size_t capacity)
{
  return std::clamp(capacity, clipboard_min_capacity, clipboard_max_capacity);
}

std::size_t clipboard_stack_step(std::size_t position, std::size_t count,
                                 ClipboardStep step)
{
  if (count == 0)
    return 0;
  const auto last = count - 1;
  const auto back = [position](std::size_t distance) {
    return position - std::min(position, distance);
  };
  switch (step) {
  case ClipboardStep::older:
    return std::min(position + 1, last);
  case ClipboardStep::newer:
    return std::min(back(1), last);
  case ClipboardStep::oldest:
    return last;
  case ClipboardStep::newest:
    return 0;
  case ClipboardStep::ten_older:
    return std::min(position + 10, last);
  case ClipboardStep::ten_newer:
    return std::min(back(10), last);
  }
  return std::min(position, last);
}

ClipboardHistory::ClipboardHistory(std::size_t capacity,
                                   std::size_t max_total_bytes)
    : capacity_(clamp_clipboard_capacity(capacity)),
      max_total_bytes_(max_total_bytes)
{
}

void ClipboardHistory::set_capacity(std::size_t capacity)
{
  capacity_ = clamp_clipboard_capacity(capacity);
  evict_while_over_limits();
}

void ClipboardHistory::evict_while_over_limits()
{
  while (entries_.size() > capacity_ ||
         (total_bytes_ > max_total_bytes_ && entries_.size() > 1)) {
    const auto &oldest = entries_.back();
    total_bytes_ -= oldest.content.size();
    entries_.pop_back();
  }
}

std::optional<std::uint64_t>
ClipboardHistory::capture(ClipboardContent content,
                          std::chrono::system_clock::time_point now)
{
  if (content.size() == 0 || content.size() > max_total_bytes_)
    return std::nullopt;
  if (!entries_.empty() && entries_.front().content == content)
    return std::nullopt;
  const auto id = next_id_++;
  total_bytes_ += content.size();
  entries_.insert(entries_.begin(),
                  ClipboardEntry{id, std::move(content), now});
  evict_while_over_limits();
  return id;
}

void ClipboardHistory::adopt(std::vector<ClipboardEntry> saved)
{
  clear();
  for (auto &entry : saved) {
    if (entry.content.size() == 0 || entry.content.size() > max_total_bytes_)
      continue;
    entry.id = next_id_++;
    total_bytes_ += entry.content.size();
    entries_.push_back(std::move(entry));
  }
  evict_while_over_limits();
}

bool ClipboardHistory::remove(std::uint64_t id)
{
  const auto entry = std::ranges::find(entries_, id, &ClipboardEntry::id);
  if (entry == entries_.end())
    return false;
  total_bytes_ -= entry->content.size();
  entries_.erase(entry);
  return true;
}

const ClipboardEntry *ClipboardHistory::find(std::uint64_t id) const
{
  for (const auto &entry : entries_)
    if (entry.id == id)
      return &entry;
  return nullptr;
}

std::optional<ClipboardContent>
ClipboardHistory::content(std::uint64_t id) const
{
  if (const auto *entry = find(id))
    return entry->content;
  return std::nullopt;
}

void ClipboardHistory::clear()
{
  entries_.clear();
  total_bytes_ = 0;
}

Collected<void>
save_clipboard_history(const std::filesystem::path &file,
                       const std::vector<ClipboardEntry> &entries)
{
  std::error_code error;
  if (file.has_parent_path()) {
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
      return std::unexpected("Cannot create the history folder");
  }
  auto temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    // Restricted while still empty, so the content is never readable by anyone
    // else.
    std::filesystem::permissions(temporary,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 error);
    if (!out || error) {
      std::filesystem::remove(temporary, error);
      return std::unexpected("Cannot create the history file");
    }
    out.write(history_magic.data(), history_magic.size());
    for (const auto &entry : entries) {
      const bool is_text = entry.content.kind == ClipboardContent::Kind::text;
      const auto captured =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              entry.captured_at.time_since_epoch());
      put(out, static_cast<std::uint8_t>(is_text ? 0 : 1));
      put(out, static_cast<std::int64_t>(captured.count()));
      put(out, static_cast<std::uint64_t>(entry.content.size()));
      out.write(
          is_text ? entry.content.text.data()
                  : reinterpret_cast<const char *>(entry.content.bytes.data()),
          static_cast<std::streamsize>(entry.content.size()));
    }
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(temporary, error);
      return std::unexpected("Cannot write the history file");
    }
  }
  std::filesystem::rename(temporary, file, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return std::unexpected("Cannot replace the history file");
  }
  return {};
}

Collected<std::vector<ClipboardEntry>>
load_clipboard_history(const std::filesystem::path &file)
{
  std::error_code error;
  const auto file_size = std::filesystem::file_size(file, error);
  if (error)
    return std::unexpected("Cannot read the history file");
  std::ifstream in(file, std::ios::binary);
  std::array<char, history_magic.size()> magic{};
  if (!in.read(magic.data(), magic.size()) || magic != history_magic)
    return std::unexpected("The history file is not recognized");

  std::vector<ClipboardEntry> entries;
  while (in.peek() != std::ifstream::traits_type::eof()) {
    std::uint8_t kind = 0;
    std::int64_t captured = 0;
    std::uint64_t size = 0;
    if (!get(in, kind) || !get(in, captured) || !get(in, size))
      return std::unexpected("The history file is cut short");
    const auto position = static_cast<std::uint64_t>(in.tellg());
    if (kind > 1 || size > file_size - position ||
        size > clipboard_max_total_bytes)
      return std::unexpected("The history file is damaged");

    ClipboardEntry entry;
    entry.captured_at = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(captured)));
    const auto length = static_cast<std::streamsize>(size);
    if (kind == 0) {
      entry.content.text.resize(size);
      in.read(entry.content.text.data(), length);
    } else {
      entry.content.kind = ClipboardContent::Kind::image;
      entry.content.bytes.resize(size);
      in.read(reinterpret_cast<char *>(entry.content.bytes.data()), length);
    }
    if (!in)
      return std::unexpected("The history file is cut short");
    entries.push_back(std::move(entry));
  }
  return entries;
}

ClipboardTracker::ClipboardTracker(ClipboardHistory &history,
                                   PasteboardSources sources)
    : history_(history), sources_(std::move(sources))
{
}

bool ClipboardTracker::poll()
{
  const auto count = sources_.change_count();
  if (count == last_seen_)
    return false;
  // An empty or unsupported pasteboard is not an error worth reporting; it is
  // just nothing new. It can also be a poll that landed inside another
  // process's write, between the clear and the new contents, when the count has
  // already moved: read this count once more on the next poll before treating
  // it as settled.
  auto content = sources_.read();
  if (!content || content->size() == 0) {
    if (retry_ == count) {
      retry_.reset();
      last_seen_ = count;
    } else {
      retry_ = count;
    }
    return false;
  }
  retry_.reset();
  last_seen_ = count;
  return history_.capture(std::move(*content), std::chrono::system_clock::now())
      .has_value();
}

void ClipboardTracker::mark_seen(std::uint64_t count)
{
  retry_.reset();
  last_seen_ = count;
}

ClipboardWatcher::ClipboardWatcher(std::function<void()> on_change,
                                   std::size_t capacity,
                                   PasteboardSources sources,
                                   std::chrono::duration<double> poll_interval,
                                   const std::filesystem::path &history_file)
    : history_(opened_history(capacity, history_file)),
      tracker_(history_, sources), history_file_(history_file),
      on_change_(std::move(on_change)), poll_interval_(poll_interval),
      sources_(std::move(sources)),
      worker_([this](const std::stop_token &stop) {
        while (!stop.stop_requested()) {
          poll_pass();
          std::unique_lock lock(wake_mutex_);
          wakeup_.wait_for(lock, stop, poll_interval_, [] { return false; });
        }
        save_if_dirty();
      })
{
}

void ClipboardWatcher::poll_pass()
{
  bool changed = false;
  {
    const std::scoped_lock lock(mutex_);
    changed = tracker_.poll();
    if (changed) {
      stack_position_ = 0;
      dirty_ = true;
    }
  }
  if (changed && on_change_)
    on_change_(); // outside the lock, so the callback may call back in
  save_if_dirty();
}

void ClipboardWatcher::save_if_dirty()
{
  const std::scoped_lock save_lock(save_mutex_);
  std::vector<ClipboardEntry> entries;
  {
    const std::scoped_lock lock(mutex_);
    if (!dirty_ || history_file_.empty())
      return;
    dirty_ = false;
    entries = history_.entries();
  }
  // A failed save is retried with the next change rather than on every poll,
  // because each try can copy the whole history. The history in memory is still
  // good.
  [[maybe_unused]] const auto saved =
      save_clipboard_history(history_file_, entries);
}

void ClipboardWatcher::set_history_file(std::filesystem::path file)
{
  const std::scoped_lock save_lock(save_mutex_);
  if (!history_file_.empty() && history_file_ != file) {
    std::error_code ignored;
    std::filesystem::remove(history_file_, ignored);
  }
  history_file_ = std::move(file);
  const std::scoped_lock lock(mutex_);
  dirty_ = !history_file_.empty();
}

void ClipboardWatcher::set_capacity(std::size_t capacity)
{
  const std::scoped_lock lock(mutex_);
  history_.set_capacity(capacity);
  keep_stack_position_in_bounds();
  dirty_ = true;
}

std::vector<ClipboardEntry> ClipboardWatcher::snapshot() const
{
  const std::scoped_lock lock(mutex_);
  return history_.entries();
}

bool ClipboardWatcher::write_back(std::uint64_t id)
{
  auto content = history_.content(id);
  if (!content)
    return false;
  if (!sources_.write(*content))
    return false;
  // Our own write raises the change counter; count it as seen so the next poll
  // does not record the restore as a new copy.
  tracker_.mark_seen(sources_.change_count());
  return true;
}

bool ClipboardWatcher::restore(std::uint64_t id)
{
  const std::scoped_lock lock(mutex_);
  return write_back(id);
}

void ClipboardWatcher::clear()
{
  const std::scoped_lock save_lock(save_mutex_);
  {
    const std::scoped_lock lock(mutex_);
    history_.clear();
    stack_position_ = 0;
    dirty_ = false;
  }
  if (!history_file_.empty()) {
    std::error_code ignored;
    std::filesystem::remove(history_file_, ignored);
  }
}

void ClipboardWatcher::keep_stack_position_in_bounds()
{
  const auto count = history_.entries().size();
  stack_position_ = count == 0 ? 0 : std::min(stack_position_, count - 1);
}

std::size_t ClipboardWatcher::stack_position() const
{
  const std::scoped_lock lock(mutex_);
  return stack_position_;
}

std::size_t ClipboardWatcher::stack_move(ClipboardStep step)
{
  const std::scoped_lock lock(mutex_);
  stack_position_ =
      clipboard_stack_step(stack_position_, history_.entries().size(), step);
  return stack_position_;
}

std::size_t ClipboardWatcher::stack_jump(std::size_t position)
{
  const std::scoped_lock lock(mutex_);
  if (position < history_.entries().size())
    stack_position_ = position;
  return stack_position_;
}

bool ClipboardWatcher::stack_restore()
{
  const std::scoped_lock lock(mutex_);
  const auto &entries = history_.entries();
  return stack_position_ < entries.size() &&
         write_back(entries[stack_position_].id);
}

void ClipboardWatcher::stack_remove()
{
  const std::scoped_lock lock(mutex_);
  const auto &entries = history_.entries();
  if (stack_position_ < entries.size()) {
    history_.remove(entries[stack_position_].id);
    keep_stack_position_in_bounds();
    dirty_ = true;
  }
}

} // namespace hm
