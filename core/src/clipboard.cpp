#include "hm/clipboard.hpp"

#include <algorithm>
#include <utility>

namespace hm {

std::size_t clamp_clipboard_capacity(std::size_t capacity) {
    return std::clamp(capacity, clipboard_min_capacity, clipboard_max_capacity);
}

ClipboardHistory::ClipboardHistory(std::size_t capacity, std::size_t max_total_bytes)
    : capacity_(clamp_clipboard_capacity(capacity)), max_total_bytes_(max_total_bytes) {}

void ClipboardHistory::set_capacity(std::size_t capacity) {
    capacity_ = clamp_clipboard_capacity(capacity);
    evict_while_over_limits();
}

void ClipboardHistory::evict_while_over_limits() {
    while (entries_.size() > capacity_ ||
           (total_bytes_ > max_total_bytes_ && entries_.size() > 1)) {
        const auto &oldest = entries_.back();
        total_bytes_ -= oldest.content.size();
        entries_.pop_back();
    }
}

std::optional<std::uint64_t> ClipboardHistory::capture(ClipboardContent content,
                                                       std::chrono::system_clock::time_point now) {
    if (content.size() == 0 || content.size() > max_total_bytes_)
        return std::nullopt;
    // A duplicate of an existing entry reuses it: the newest one stays put, an older one moves to
    // the front. That keeps one copy per distinct payload, in most-recently-used order.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].content == content) {
            if (i == 0)
                return std::nullopt;
            auto entry = std::move(entries_[i]);
            entry.captured_at = now;
            const auto id = entry.id;
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            entries_.insert(entries_.begin(), std::move(entry));
            return id;
        }
    }
    const auto id = next_id_++;
    total_bytes_ += content.size();
    entries_.insert(entries_.begin(), ClipboardEntry{id, std::move(content), now});
    evict_while_over_limits();
    return id;
}

bool ClipboardHistory::promote(std::uint64_t id, std::chrono::system_clock::time_point now) {
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            auto entry = std::move(entries_[i]);
            entry.captured_at = now;
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            entries_.insert(entries_.begin(), std::move(entry));
            return true;
        }
    }
    return !entries_.empty() && entries_.front().id == id;
}

const ClipboardEntry *ClipboardHistory::find(std::uint64_t id) const {
    for (const auto &entry : entries_)
        if (entry.id == id)
            return &entry;
    return nullptr;
}

std::optional<ClipboardContent> ClipboardHistory::content(std::uint64_t id) const {
    if (const auto *entry = find(id))
        return entry->content;
    return std::nullopt;
}

void ClipboardHistory::clear() {
    entries_.clear();
    total_bytes_ = 0;
}

ClipboardTracker::ClipboardTracker(ClipboardHistory &history, PasteboardSources sources)
    : history_(history), sources_(std::move(sources)) {}

bool ClipboardTracker::poll() {
    const auto count = sources_.change_count();
    if (count == last_seen_)
        return false;
    // An empty or unsupported pasteboard is not an error worth reporting; it is just nothing
    // new. It can also be a poll that landed inside another process's write, between the clear
    // and the new contents, when the count has already moved: read this count once more on the
    // next poll before treating it as settled.
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
    return history_.capture(std::move(*content), std::chrono::system_clock::now()).has_value();
}

void ClipboardTracker::mark_seen(std::uint64_t count) {
    retry_.reset();
    last_seen_ = count;
}

ClipboardWatcher::ClipboardWatcher(std::function<void()> on_change, std::size_t capacity,
                                   PasteboardSources sources,
                                   std::chrono::duration<double> poll_interval)
    : history_(capacity), tracker_(history_, sources), on_change_(std::move(on_change)),
      poll_interval_(poll_interval), sources_(std::move(sources)),
      worker_([this](const std::stop_token &stop) {
          while (!stop.stop_requested()) {
              poll_pass();
              std::unique_lock lock(wake_mutex_);
              wakeup_.wait_for(lock, stop, poll_interval_, [] { return false; });
          }
      }) {}

void ClipboardWatcher::poll_pass() {
    bool changed = false;
    {
        const std::scoped_lock lock(mutex_);
        changed = tracker_.poll();
    }
    if (changed && on_change_)
        on_change_(); // outside the lock, so the callback may call back in
}

void ClipboardWatcher::set_capacity(std::size_t capacity) {
    const std::scoped_lock lock(mutex_);
    history_.set_capacity(capacity);
}

std::vector<ClipboardEntry> ClipboardWatcher::snapshot() const {
    const std::scoped_lock lock(mutex_);
    return history_.entries();
}

bool ClipboardWatcher::restore(std::uint64_t id) {
    const std::scoped_lock lock(mutex_);
    auto content = history_.content(id);
    if (!content)
        return false;
    if (!sources_.write(*content))
        return false;
    // Our own write raises the change counter; count it as seen so the next poll does not record
    // the restore as a new copy.
    tracker_.mark_seen(sources_.change_count());
    history_.promote(id, std::chrono::system_clock::now());
    return true;
}

void ClipboardWatcher::clear() {
    const std::scoped_lock lock(mutex_);
    history_.clear();
}

} // namespace hm
