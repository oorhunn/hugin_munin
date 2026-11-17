// The copy/paste buffer logic, driven by a fake pasteboard; only the last group
// uses a real thread through ClipboardWatcher.
#include "check.hpp"
#include "hm/clipboard.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  const auto repeated = history.capture(text("alpha"), now);
  require(repeated && *repeated != *a && history.entries().size() == 3,
          "A duplicate of an older entry is a new entry");
  require(history.remove(*repeated), "Remove finds an entry");
  require(!history.remove(*repeated), "Remove rejects an unknown id");
  require(history.entries().size() == 2 && history.entries().front().id == *b,
          "Remove leaves the other entries in order");

  const auto c = history.capture(text("gamma"), now);
  require(c && history.entries().size() == 3, "Capacity 3 holds three entries");
  const auto d = history.capture(text("delta"), now);
  require(d && history.entries().size() == 3,
          "Capacity 3 still holds three entries");
  require(history.entries().back().content.text == "beta",
          "The oldest entry is evicted first");
  require(!history.content(*a), "An evicted entry is gone");

  require(!history.capture(text(""), now), "Empty content is not recorded");
  require(!history.capture(image(std::vector<std::uint8_t>(2000, 7)), now),
          "Content over the byte budget is not recorded");
  require(history.entries().size() == 3, "A rejected capture changes nothing");

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

void stack_steps()
{
  using enum hm::ClipboardStep;
  require(hm::clipboard_stack_step(0, 0, older) == 0,
          "An empty history stays at 0");
  require(hm::clipboard_stack_step(0, 3, older) == 1,
          "Older moves away from the newest");
  require(hm::clipboard_stack_step(2, 3, older) == 2,
          "Older stops at the oldest entry");
  require(hm::clipboard_stack_step(1, 3, newer) == 0,
          "Newer moves toward the newest");
  require(hm::clipboard_stack_step(0, 3, newer) == 0,
          "Newer stops at the newest entry");
  require(hm::clipboard_stack_step(1, 30, oldest) == 29,
          "Oldest goes to the last entry");
  require(hm::clipboard_stack_step(9, 30, newest) == 0,
          "Newest goes to the first entry");
  require(hm::clipboard_stack_step(5, 30, ten_older) == 15,
          "A page moves ten entries");
  require(hm::clipboard_stack_step(25, 30, ten_older) == 29,
          "A page stops at the oldest");
  require(hm::clipboard_stack_step(15, 30, ten_newer) == 5,
          "A page moves back ten entries");
  require(hm::clipboard_stack_step(5, 30, ten_newer) == 0,
          "A page stops at the newest");
}

void wait_for_changes(const std::atomic<int> &changes, int target)
{
  for (int i = 0; i < 600 && changes < target; ++i)
    std::this_thread::sleep_for(5ms);
  require(changes.load() == target, "The watcher saw the expected copies");
}

void watcher_stack()
{
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms);
  wait_for_changes(changes, 1);
  pasteboard.copy(2, text("two"));
  wait_for_changes(changes, 2);
  pasteboard.copy(3, text("three"));
  wait_for_changes(changes, 3);

  using enum hm::ClipboardStep;
  require(watcher.stack_position() == 0,
          "The stack starts at the newest entry");
  require(watcher.stack_move(older) == 1, "The stack moves to an older entry");
  require(watcher.stack_restore(), "The entry at the stack position restores");
  require(pasteboard.written().back().text == "two",
          "The stack position picks the entry");
  require(watcher.stack_position() == 1, "Pasting keeps the stack position");
  require(watcher.snapshot().front().content.text == "three",
          "Pasting keeps the order");

  require(watcher.stack_jump(2) == 2, "A jump goes to an existing position");
  require(watcher.stack_jump(9) == 2, "A jump past the end stays put");
  watcher.stack_remove();
  require(watcher.snapshot().size() == 2 && watcher.stack_position() == 1,
          "Removing the last entry keeps the position valid");

  pasteboard.copy(10, text("four"));
  wait_for_changes(changes, 4);
  require(watcher.stack_position() == 0,
          "A new copy returns to the newest entry");

  watcher.stack_move(oldest);
  watcher.set_capacity(1);
  require(watcher.stack_position() == 0,
          "A smaller capacity keeps the position valid");
  watcher.clear();
  require(!watcher.stack_restore(), "An empty history has nothing to restore");
  watcher.stack_remove();
  require(watcher.stack_position() == 0,
          "Removing from an empty history is safe");
}

void history_file()
{
  namespace fs = std::filesystem;
  const auto folder =
      fs::temp_directory_path() /
      ("hm-clipboard-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto file = folder / "nested" / "history.bin";
  const auto now = std::chrono::system_clock::now();

  require(!hm::load_clipboard_history(file), "A missing file is an error");
  const std::vector<hm::ClipboardEntry> entries{
      {.id = 8, .content = text("new"), .captured_at = now},
      {.id = 3, .content = image({1, 0, 3}), .captured_at = now}};
  require(hm::save_clipboard_history(file, entries).has_value(),
          "The history saves");
  require((fs::status(file).permissions() & fs::perms::all) ==
              (fs::perms::owner_read | fs::perms::owner_write),
          "Only the owner can read the file");
  const auto loaded = hm::load_clipboard_history(file);
  require(loaded && loaded->size() == 2 &&
              loaded->front().content == text("new") &&
              loaded->back().content == image({1, 0, 3}),
          "The history loads in order, with binary content intact");
  require(std::chrono::floor<std::chrono::milliseconds>(
              loaded->front().captured_at) ==
              std::chrono::floor<std::chrono::milliseconds>(now),
          "The capture time survives");

  const auto size = fs::file_size(file);
  fs::resize_file(file, size - 1);
  require(!hm::load_clipboard_history(file), "A cut-off file is an error");
  std::ofstream(file, std::ios::binary | std::ios::trunc) << "not a history";
  require(!hm::load_clipboard_history(file), "A foreign file is an error");
  fs::remove(file);

  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  {
    hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                                 pasteboard.sources(), 5ms, file);
    wait_for_changes(changes, 1);
    pasteboard.copy(2, text("two"));
    wait_for_changes(changes, 2);
  }
  require(fs::exists(file), "The watcher saves its history");

  {
    changes = 0;
    hm::ClipboardWatcher reopened([&changes] { ++changes; }, 10,
                                  pasteboard.sources(), 5ms, file);
    const auto snapshot = reopened.snapshot();
    require(snapshot.size() == 2 && snapshot.front().content.text == "two" &&
                snapshot.back().content.text == "one",
            "A restart brings the history back in order");
    for (int i = 0; i < 40; ++i)
      std::this_thread::sleep_for(5ms);
    require(reopened.snapshot().size() == 2,
            "The pasteboard at launch is not recorded again when it is already "
            "newest");
    reopened.clear();
    require(!fs::exists(file), "Clear deletes the file at once");
  }

  {
    hm::ClipboardWatcher memory_only(nullptr, 10, pasteboard.sources(), 5ms);
    pasteboard.copy(3, text("three"));
    for (int i = 0; i < 600 && memory_only.snapshot().empty(); ++i)
      std::this_thread::sleep_for(5ms);
    memory_only.set_history_file(file);
    for (int i = 0; i < 600 && !fs::exists(file); ++i)
      std::this_thread::sleep_for(5ms);
    require(fs::exists(file), "Turning saving on writes the current history");
    memory_only.set_history_file({});
    require(!fs::exists(file), "Turning saving off deletes the file");
  }
  require(!fs::exists(file),
          "A watcher that stopped saving leaves no file behind");

  hm::ClipboardHistory small(1);
  small.adopt({{.id = 1, .content = text("newest"), .captured_at = now},
               {.id = 2, .content = text("oldest"), .captured_at = now}});
  require(small.entries().size() == 1 &&
              small.entries().front().content.text == "newest",
          "A saved history respects a smaller capacity");

  fs::remove_all(folder);
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
  require(watcher.snapshot().back().id == one_id,
          "Restore leaves the history order alone");
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

// Polls `condition` for up to a few seconds; the watcher's thread does the
// work.
template <typename Condition>
bool wait_until(Condition condition, int milliseconds = 4000)
{
  for (int waited = 0; waited < milliseconds; waited += 5) {
    if (condition())
      return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

// A folder that is removed when it goes out of scope.
struct TempFolder {
  std::filesystem::path path;

  explicit TempFolder(const std::string &name)
      : path(std::filesystem::temp_directory_path() /
             ("hm-clipboard-" + name + "-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count())))
  {
    std::filesystem::create_directories(path);
  }
  ~TempFolder()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TempFolder(const TempFolder &) = delete;
  TempFolder &operator=(const TempFolder &) = delete;
};

// Retries that run out within a few milliseconds.
constexpr hm::ClipboardRetryPolicy quick_retries{
    .max_automatic_retries = 3, .first_delay = 2ms, .max_delay = 10ms};
// Retries that effectively never run out.
constexpr hm::ClipboardRetryPolicy endless_retries{
    .max_automatic_retries = 1000000, .first_delay = 2ms, .max_delay = 5ms};

// A history file below a regular file cannot be created, whatever the user or
// permissions: the parent is not a folder. Replacing the blocker with a folder
// is the "fix".
struct BlockedPath {
  std::filesystem::path blocker;
  std::filesystem::path file;

  explicit BlockedPath(const TempFolder &folder)
      : blocker(folder.path / "blocker"), file(blocker / "history.bin")
  {
    std::ofstream(blocker) << "not a folder";
  }
  void fix() const
  {
    std::filesystem::remove(blocker);
    std::filesystem::create_directory(blocker);
  }
};

std::vector<std::string>
texts_of(const std::vector<hm::ClipboardEntry> &entries)
{
  std::vector<std::string> texts;
  texts.reserve(entries.size());
  for (const auto &entry : entries)
    texts.push_back(entry.content.text);
  return texts;
}

void persistence_save_failure()
{
  namespace fs = std::filesystem;
  TempFolder folder("save-failure");
  const BlockedPath blocked(folder);
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;

  hm::ClipboardPersistenceStatus failed;
  {
    hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                                 pasteboard.sources(), 5ms, blocked.file,
                                 quick_retries);
    wait_for_changes(changes, 1);
    require(
        wait_until([&] { return watcher.persistence_status().save_failed; }),
        "A save that cannot create its file is reported");
    require(wait_until(
                [&] { return !watcher.persistence_status().retry_scheduled; }),
            "Automatic retries are bounded");
    failed = watcher.persistence_status();
    require(failed.save_failed && !failed.delete_failed &&
                !failed.retry_scheduled,
            "An exhausted save failure asks for an explicit retry");

    // The history in memory is still usable.
    const auto snapshot = watcher.snapshot();
    require(snapshot.size() == 1 && watcher.restore(snapshot.front().id) &&
                pasteboard.written().back().text == "one",
            "The history stays usable while saving fails");

    // Nothing changes by itself once the retries are spent, even after the
    // cause is gone.
    blocked.fix();
    std::this_thread::sleep_for(100ms);
    require(!fs::exists(blocked.file) &&
                watcher.persistence_status().generation == failed.generation &&
                watcher.persistence_status().save_failed,
            "Spent retries do not run again on their own");

    // Recovery with no new entry: an explicit retry saves what was kept.
    watcher.request_persistence_retry();
    require(
        wait_until([&] { return !watcher.persistence_status().save_failed; }),
        "An explicit retry recovers without a new entry");
    const auto recovered = watcher.persistence_status();
    require(!recovered.save_failed && !recovered.delete_failed &&
                !recovered.retry_scheduled &&
                recovered.generation > failed.generation,
            "A successful retry clears the status");
    const auto saved = hm::load_clipboard_history(blocked.file);
    require(saved && texts_of(*saved) == std::vector<std::string>{"one"},
            "The retried save holds the history");
    require(changes.load() == 1, "Retrying does not notify");
  }
}

void persistence_automatic_retry()
{
  TempFolder folder("auto-retry");
  const BlockedPath blocked(folder);
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms, blocked.file,
                               endless_retries);
  wait_for_changes(changes, 1);
  require(wait_until([&] { return watcher.persistence_status().save_failed; }),
          "The failure shows while retries remain");
  require(watcher.persistence_status().retry_scheduled,
          "A failure with retries left says one is scheduled");

  blocked.fix();
  require(wait_until([&] { return !watcher.persistence_status().save_failed; }),
          "A scheduled retry recovers without a new entry or a request");
  require(!watcher.persistence_status().retry_scheduled &&
              hm::load_clipboard_history(blocked.file).has_value() &&
              changes.load() == 1,
          "Recovery leaves nothing scheduled");
}

void persistence_new_entry_retries()
{
  namespace fs = std::filesystem;
  TempFolder folder("new-entry");
  const BlockedPath blocked(folder);
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms, blocked.file,
                               quick_retries);
  wait_for_changes(changes, 1);
  require(wait_until([&] {
            const auto status = watcher.persistence_status();
            return status.save_failed && !status.retry_scheduled;
          }),
          "The retries are spent");

  blocked.fix();
  pasteboard.copy(2, text("two"));
  wait_for_changes(changes, 2);
  require(wait_until([&] { return !watcher.persistence_status().save_failed; }),
          "A new entry gets a fresh try after the retries ran out");
  const auto saved = hm::load_clipboard_history(blocked.file);
  require(saved && texts_of(*saved) == std::vector<std::string>{"two", "one"},
          "The file holds the entry that failed to save and the new one");
  require(fs::exists(blocked.file), "The history file exists");
}

void persistence_stop_tries_once_more()
{
  namespace fs = std::filesystem;
  TempFolder folder("stop");
  const BlockedPath blocked(folder);
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  {
    hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                                 pasteboard.sources(), 5ms, blocked.file,
                                 quick_retries);
    wait_for_changes(changes, 1);
    require(wait_until([&] {
              const auto status = watcher.persistence_status();
              return status.save_failed && !status.retry_scheduled;
            }),
            "The retries are spent");
    blocked.fix();
    std::this_thread::sleep_for(100ms);
    require(!fs::exists(blocked.file), "Nothing saved yet");
  }
  const auto saved = hm::load_clipboard_history(blocked.file);
  require(saved && texts_of(*saved) == std::vector<std::string>{"one"},
          "Stopping tries the unsaved history once more");
}

void persistence_switching_after_failure()
{
  namespace fs = std::filesystem;
  TempFolder folder("switch");
  const BlockedPath blocked(folder);
  const auto good = folder.path / "good" / "history.bin";
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms, blocked.file,
                               quick_retries);
  wait_for_changes(changes, 1);
  require(wait_until([&] {
            const auto status = watcher.persistence_status();
            return status.save_failed && !status.retry_scheduled;
          }),
          "The retries are spent");

  watcher.set_history_file(good);
  require(wait_until([&] { return fs::exists(good); }) && wait_until([&] {
            return !watcher.persistence_status().save_failed;
          }),
          "A new history file starts with a fresh try");
  const auto status = watcher.persistence_status();
  require(!status.save_failed && !status.delete_failed &&
              !status.retry_scheduled,
          "A file that never existed is not a failed deletion");

  // Turning saving off after a failed save has nothing to delete.
  watcher.set_history_file(blocked.file);
  require(wait_until([&] { return watcher.persistence_status().save_failed; }),
          "Saving to the blocked path fails again");
  watcher.set_history_file({});
  const auto off = watcher.persistence_status();
  require(!off.save_failed && !off.delete_failed && !off.retry_scheduled,
          "Turning saving off clears a failed save");
  require(!fs::exists(good), "Moving away from a file deletes it");
}

void persistence_delete_failure()
{
  namespace fs = std::filesystem;
  TempFolder folder("delete-failure");
  // A folder that still holds something cannot be removed as a file.
  const auto stuck = folder.path / "stuck";
  fs::create_directories(stuck);
  std::ofstream(stuck / "keep") << "x";

  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms, {}, quick_retries);
  wait_for_changes(changes, 1);
  require(watcher.persistence_status().generation == 0 &&
              !watcher.persistence_status().save_failed,
          "A memory-only watcher has nothing to report");

  watcher.set_history_file(stuck);
  watcher.set_history_file({});
  const auto failed = watcher.persistence_status();
  require(failed.delete_failed && !failed.save_failed,
          "A deletion that fails is reported, not claimed");
  require(fs::exists(stuck / "keep"),
          "The file that could not go is still there");
  require(
      wait_until([&] { return !watcher.persistence_status().retry_scheduled; }),
      "Deletion retries are bounded");
  require(watcher.persistence_status().delete_failed,
          "A spent deletion retry still reports the leftover file");

  // Saving stays off while the deletion is pending.
  pasteboard.copy(2, text("two"));
  wait_for_changes(changes, 2);
  std::this_thread::sleep_for(100ms);
  require(fs::exists(stuck / "keep") && !fs::exists(stuck.string() + ".tmp") &&
              !watcher.persistence_status().save_failed,
          "A failed disable does not resume saving");

  // The retry target survived: once the obstacle is gone, a retry finishes it.
  fs::remove(stuck / "keep");
  const auto before = watcher.persistence_status().generation;
  watcher.request_persistence_retry();
  require(
      wait_until([&] { return !watcher.persistence_status().delete_failed; }),
      "A retry finishes the pending deletion");
  require(!fs::exists(stuck), "The pending file is deleted");
  const auto cleared = watcher.persistence_status();
  require(!cleared.save_failed && !cleared.retry_scheduled &&
              cleared.generation > before,
          "A finished deletion clears the status");

  pasteboard.copy(3, text("three"));
  wait_for_changes(changes, 3);
  std::this_thread::sleep_for(100ms);
  require(!fs::exists(stuck), "Saving stayed off after the deletion");
}

void persistence_clear_failure()
{
  namespace fs = std::filesystem;
  TempFolder folder("clear-failure");
  const auto file = folder.path / "history.bin";
  FakePasteboard pasteboard;
  pasteboard.copy(1, text("one"));
  std::atomic<int> changes = 0;
  hm::ClipboardWatcher watcher([&changes] { ++changes; }, 10,
                               pasteboard.sources(), 5ms, file, quick_retries);
  wait_for_changes(changes, 1);
  require(wait_until([&] { return fs::exists(file); }),
          "The history file exists before clearing");

  // Swap the file for a folder that still holds something.
  fs::remove(file);
  fs::create_directories(file / "keep");
  watcher.clear();
  require(watcher.snapshot().empty(), "Clear empties the history regardless");
  const auto failed = watcher.persistence_status();
  require(failed.delete_failed && !failed.save_failed && fs::exists(file),
          "Clear does not claim a deletion that failed");

  fs::remove_all(file / "keep");
  watcher.request_persistence_retry();
  require(
      wait_until([&] { return !watcher.persistence_status().delete_failed; }),
      "A retry finishes the deletion after clear");
  require(!fs::exists(file), "The history file is gone");

  // Saving carried on: the next copy writes the file again.
  pasteboard.copy(2, text("two"));
  require(wait_until([&] { return fs::exists(file); }),
          "Saving continues after a clear");
  const auto saved = hm::load_clipboard_history(file);
  require(saved && texts_of(*saved) == std::vector<std::string>{"two"},
          "Only the new history is saved after a clear");
  require(!watcher.persistence_status().delete_failed,
          "A later save leaves no deletion pending");
}

// Copies arrive while saves run, and settings change on another thread.
// Whatever the interleaving, the file must end up matching the history.
void persistence_concurrent_changes()
{
  TempFolder folder("concurrent");
  const auto file = folder.path / "history.bin";
  FakePasteboard pasteboard;
  std::vector<std::string> final_texts;
  {
    hm::ClipboardWatcher watcher(nullptr, 500, pasteboard.sources(), 1ms, file);
    std::atomic<bool> done = false;
    std::thread settings([&] {
      for (std::size_t round = 0; !done; ++round) {
        watcher.set_capacity(450 + round % 50);
        (void)watcher.persistence_status();
        std::this_thread::sleep_for(1ms);
      }
    });
    for (std::uint64_t i = 1; i <= 200; ++i) {
      pasteboard.copy(i, text("copy " + std::to_string(i)));
      std::this_thread::sleep_for(2ms);
    }
    const bool settled = wait_until([&] {
      const auto snapshot = watcher.snapshot();
      return !snapshot.empty() && snapshot.front().content.text == "copy 200";
    });
    done = true;
    settings.join();
    require(settled, "The watcher saw the last copy");
    final_texts = texts_of(watcher.snapshot());
  }
  const auto saved = hm::load_clipboard_history(file);
  require(saved && texts_of(*saved) == final_texts,
          "Changes made during saves are not lost");
}

} // namespace

int main()
{
  history_rules();
  byte_budget();
  image_rules();
  tracker_rules();
  stack_steps();
  watcher_stack();
  history_file();
  watcher_thread();
  persistence_save_failure();
  persistence_automatic_retry();
  persistence_new_entry_retries();
  persistence_stop_tries_once_more();
  persistence_switching_after_failure();
  persistence_delete_failure();
  persistence_clear_failure();
  persistence_concurrent_changes();
  return 0;
}
