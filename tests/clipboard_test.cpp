// The copy/paste buffer logic, driven by a fake pasteboard; only the last group
// uses a real thread through ClipboardWatcher.
#include "check.hpp"
#include "hm/clipboard.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using Time = std::chrono::system_clock::time_point;

hm::ClipboardContent text(std::string value)
{
  return {.kind = hm::ClipboardContent::Kind::text, .text = std::move(value)};
}

hm::ClipboardContent image(std::vector<std::uint8_t> bytes)
{
  return {.kind = hm::ClipboardContent::Kind::image, .bytes = std::move(bytes)};
}

// Stands in for the system pasteboard. A write bumps the change count, as the
// real one does. The system synchronizes the real pasteboard, so the fake needs
// its own mutex: the tests change it while the watcher thread reads it.
struct FakePasteboard {
  std::mutex mutex;
  std::uint64_t count = 1;
  hm::ClipboardContent current = text("");
  bool fail_read = false;
  std::vector<hm::ClipboardContent> writes;

  hm::PasteboardSources sources()
  {
    return {[this] {
              const std::scoped_lock lock(mutex);
              return count;
            },
            [this]() -> hm::Collected<hm::ClipboardContent> {
              const std::scoped_lock lock(mutex);
              if (fail_read || current.size() == 0)
                return std::unexpected("nothing on the pasteboard");
              return current;
            },
            [this](const hm::ClipboardContent &content) -> hm::Collected<void> {
              const std::scoped_lock lock(mutex);
              writes.push_back(content);
              ++count;
              return {};
            }};
  }

  /// Simulates a copy: new contents at a new change count.
  void copy(std::uint64_t new_count, hm::ClipboardContent content)
  {
    const std::scoped_lock lock(mutex);
    count = new_count;
    current = std::move(content);
  }

  std::vector<hm::ClipboardContent> written()
  {
    const std::scoped_lock lock(mutex);
    return writes;
  }
};

void history_rules()
{
  const auto now = std::chrono::system_clock::now();
  hm::ClipboardHistory history(3, 1000);

  const auto a = history.capture(text("alpha"), now);
  const auto b = history.capture(text("beta"), now);
  require(a && b && *a != *b, "Two captures return distinct ids");
  require(history.entries().size() == 2 &&
              history.entries().front().content.text == "beta",
          "The newest entry is first");

  require(!history.capture(text("beta"), now),
          "A duplicate of the newest entry is ignored");
  require(history.capture(text("alpha"), now) == a,
          "A duplicate of an older entry reuses it");
  require(history.entries().size() == 2 && history.entries().front().id == *a,
          "A duplicate of an older entry moves it to the front");

  const auto c = history.capture(text("gamma"), now);
  require(c && history.entries().size() == 3, "Capacity 3 holds three entries");
  const auto d = history.capture(text("delta"), now);
  require(d && history.entries().size() == 3,
          "Capacity 3 still holds three entries");
  require(history.entries().back().content.text == "alpha",
          "The oldest entry is evicted first");
  require(!history.content(*b), "An evicted entry is gone");

  require(!history.capture(text(""), now), "Empty content is not recorded");
  require(!history.capture(image(std::vector<std::uint8_t>(2000, 7)), now),
          "Content over the byte budget is not recorded");
  require(history.entries().size() == 3, "A rejected capture changes nothing");

  require(history.promote(history.entries().back().id, now),
          "Promote finds an older entry");
  const auto promoted = history.entries().front().id;
  require(history.entries().size() == 3, "Promote keeps every entry");
  require(history.promote(promoted, now), "Promote accepts the front entry");
  require(!history.promote(9999, now), "Promote rejects an unknown id");

  history.set_capacity(2);
  require(history.entries().size() == 2 && history.total_bytes() <= 1000,
          "Lowering the capacity drops the oldest entries");
  history.set_capacity(0);
  require(history.capacity() == 1 && history.entries().size() == 1,
          "Capacity is at least 1");
  hm::ClipboardHistory huge(100000);
  require(huge.capacity() == 500, "Capacity is at most 500");
  require(hm::clamp_clipboard_capacity(hm::clipboard_default_capacity) == 50,
          "The default capacity is 50");

  history.clear();
  require(history.entries().empty() && history.total_bytes() == 0,
          "Clear empties the history");
  const auto again = history.capture(text("beta"), now);
  require(again && *again != *b, "Ids are never reused");
}

void byte_budget()
{
  const auto now = std::chrono::system_clock::now();
  hm::ClipboardHistory history(10, 100);

  history.capture(text(std::string(50, 'a')), now);
  history.capture(text(std::string(50, 'b')), now);
  require(history.entries().size() == 2 && history.total_bytes() == 100,
          "Two entries at the byte budget");
  history.capture(text(std::string(30, 'c')), now);
  require(history.entries().size() == 2 && history.total_bytes() == 80,
          "Going over the budget evicts the oldest entry");
  require(history.entries().front().content.text == std::string(30, 'c') &&
              history.entries().back().content.text == std::string(50, 'b'),
          "The budget keeps the newest entries");
}

void image_rules()
{
  const auto now = std::chrono::system_clock::now();
  hm::ClipboardHistory history(5, 1000);

  const auto first = history.capture(image({1, 2, 3}), now);
  require(first.has_value(), "An image is recorded");
  require(history.entries().front().content.kind ==
              hm::ClipboardContent::Kind::image,
          "The image kind is kept");
  require(!history.capture(image({1, 2, 3}), now),
          "Identical image bytes are a duplicate");
  const auto second = history.capture(image({1, 2, 4}), now);
  require(second && *second != *first, "Different bytes are a different image");

  // Same bytes, different kind: text is never a duplicate of an image.
  const auto as_text = history.capture(text("\x01\x02\x03"), now);
  require(as_text && *as_text != *first,
          "Text with an image's bytes is a different entry");
  require(history.entries().size() == 3, "Kinds do not mix in comparisons");
}

void tracker_rules()
{
  FakePasteboard pasteboard;
  hm::ClipboardHistory history(10);
  hm::ClipboardTracker tracker(history, pasteboard.sources());

  pasteboard.current = text("first");
  require(tracker.poll(), "The first pass seeds the history");
  require(history.entries().size() == 1 &&
              history.entries().front().content.text == "first",
          "The seeded entry matches the pasteboard");

  require(!tracker.poll(), "An unchanged pasteboard is not read again");

  pasteboard.count = 2;
  require(!tracker.poll(),
          "The same content copied again is not recorded twice");

  pasteboard.current = text("second");
  pasteboard.count = 3;
  require(tracker.poll(), "New content is recorded");

  pasteboard.current = text("");
  pasteboard.count = 4;
  require(!tracker.poll(), "A cleared pasteboard is not an error");
  require(!tracker.poll(), "A cleared pasteboard settles after one retry");

  pasteboard.fail_read = true;
  pasteboard.count = 5;
  require(!tracker.poll(), "A failed read is not an error");
  pasteboard.fail_read = false;
  pasteboard.current = text("second");
  require(!tracker.poll(),
          "A retried read records what the pasteboard now holds");
  pasteboard.current = text("third");
  pasteboard.count = 6;
  require(tracker.poll(), "A later count is still read after a failure");

  // A poll can land inside another process's write, after the clear and before
  // the new contents: the count has moved, but the data is not there yet.
  pasteboard.current = text("");
  pasteboard.count = 7;
  require(!tracker.poll(), "A mid-write poll records nothing");
  pasteboard.current = text("fourth");
  require(tracker.poll(), "The retry picks up the completed write");

  // The watcher marks its own writes as seen, so a restore is not recorded as a
  // new copy.
  tracker.mark_seen(8);
  pasteboard.count = 8;
  require(!tracker.poll(), "A count marked seen is not read");
}

void watcher_thread()
{
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms);

  for (int i = 0; i < 600 && changes < 1; ++i)
    std::this_thread::sleep_for(5ms);
  require(changes.load() == 1,
          "The pasteboard at launch is seeded with one notification");

  pasteboard.copy(2, text("two"));
  for (int i = 0; i < 600 && changes < 2; ++i)
    std::this_thread::sleep_for(5ms);
  require(changes.load() == 2, "A copy notifies once");
  {
    const auto snapshot = watcher.snapshot();
    require(snapshot.size() == 2 && snapshot.front().content.text == "two",
            "The watcher snapshots the newest entry first");
  }

  const auto one_id = watcher.snapshot().back().id;
  require(watcher.restore(one_id), "Restore finds the older entry");
  const auto writes = pasteboard.written();
  require(writes.size() == 1 && writes.front().text == "one",
          "Restore writes the entry to the pasteboard");
  require(watcher.snapshot().front().id == one_id,
          "Restore moves the entry to the front");
  for (int i = 0; i < 100; ++i)
    std::this_thread::sleep_for(5ms);
  require(
      changes.load() == 2,
      "The watcher's own write does not notify and is not recorded as a copy");

  watcher.set_capacity(1);
  require(watcher.snapshot().size() == 1,
          "A live capacity change drops old entries");

  watcher.clear();
  require(watcher.snapshot().empty(), "Clear empties the watcher's history");
}

} // namespace

int main()
{
  history_rules();
  byte_budget();
  image_rules();
  tracker_rules();
  watcher_thread();
  return 0;
}
