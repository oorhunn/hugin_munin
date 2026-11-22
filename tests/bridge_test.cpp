// The C interface the Swift UI uses, against the live system.
#include "check.hpp"
#include "hm/clipboard.hpp"
#include "hm/pasteboard.hpp"
#include "hm_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
struct Received {
  std::atomic<int> calls = 0;
  hm_sample last{};
};

void receive(const hm_sample *sample, void *context)
{
  auto *received = static_cast<Received *>(context);
  received->last = *sample;
  ++received->calls;
}

// Persistence failures cross the C interface as plain flags. The watcher is
// started without a history file and pointed at paths that fail on purpose, so
// the live pasteboard decides nothing here.
void clipboard_persistence()
{
  namespace fs = std::filesystem;
  const auto none = hm_clipboard_persistence_state(nullptr);
  require(!none.save_failed && !none.delete_failed && !none.retry_scheduled &&
              none.generation == 0,
          "The persistence state of a missing watcher is empty");
  hm_clipboard_retry_persistence(nullptr);

  const auto folder =
      fs::temp_directory_path() /
      ("hm-bridge-persistence-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(folder);
  const auto wait_until = [](auto condition) {
    for (int i = 0; i < 500 && !condition(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return condition();
  };

  auto *clipboard = hm_clipboard_start([](void *) {}, nullptr, 3, nullptr);
  require(clipboard != nullptr, "The clipboard watcher starts for persistence");
  const auto quiet = hm_clipboard_persistence_state(clipboard);
  require(!quiet.save_failed && !quiet.delete_failed,
          "A watcher without a history file has nothing to report");

  // A history file below a regular file cannot be created.
  const auto blocker = folder / "blocker";
  std::ofstream(blocker) << "not a folder";
  const auto blocked = (blocker / "history.bin").string();
  require(hm_clipboard_set_history_file(clipboard, blocked.c_str()),
          "Switching saving on succeeds even when saving will fail");
  require(wait_until([&] {
            return hm_clipboard_persistence_state(clipboard).save_failed;
          }),
          "A failed save is visible through the bridge");
  const auto failed = hm_clipboard_persistence_state(clipboard);
  require(!failed.delete_failed && failed.generation > 0,
          "A failed save is not a failed deletion");

  fs::remove(blocker);
  fs::create_directory(blocker);
  hm_clipboard_retry_persistence(clipboard);
  require(wait_until([&] {
            return !hm_clipboard_persistence_state(clipboard).save_failed;
          }),
          "An explicit retry recovers through the bridge");
  require(hm_clipboard_persistence_state(clipboard).generation >
                  failed.generation &&
              fs::exists(blocker / "history.bin"),
          "The retried save wrote the file");

  // A folder that is not empty cannot be deleted as a file.
  const auto stuck = folder / "stuck";
  fs::create_directories(stuck);
  std::ofstream(stuck / "keep") << "x";
  require(hm_clipboard_set_history_file(clipboard, stuck.string().c_str()),
          "Switching to another history path succeeds");
  require(hm_clipboard_set_history_file(clipboard, nullptr),
          "Turning saving off succeeds even when the deletion fails");
  require(hm_clipboard_persistence_state(clipboard).delete_failed,
          "A failed deletion is visible through the bridge");
  require(fs::exists(stuck / "keep"), "The file that could not go remains");

  fs::remove(stuck / "keep");
  hm_clipboard_retry_persistence(clipboard);
  require(wait_until([&] {
            return !hm_clipboard_persistence_state(clipboard).delete_failed;
          }),
          "A retry finishes the deletion through the bridge");
  require(!fs::exists(stuck), "The pending deletion happened");

  hm_clipboard_stop(clipboard);
  std::error_code ignored;
  fs::remove_all(folder, ignored);
}

// The clipboard watcher against the live pasteboard. These checks replace the
// clipboard's contents, which is unavoidable: there is no fake pasteboard
// behind the C interface.
void clipboard_round_trip()
{
  hm_clipboard_stop(nullptr);
  hm_clipboard_set_capacity(nullptr, 10);
  hm_clipboard_clear(nullptr);
  require(!hm_clipboard_restore(nullptr, 1),
          "Restore without a watcher fails safely");
  require(!hm_clipboard_stack_restore(nullptr) &&
              hm_clipboard_stack_position(nullptr) == 0 &&
              hm_clipboard_stack_move(nullptr, HM_CLIPBOARD_STEP_OLDER) == 0 &&
              hm_clipboard_stack_jump(nullptr, 1) == 0,
          "The stack calls accept a missing watcher");
  hm_clipboard_stack_remove(nullptr);
  require(!hm_clipboard_set_history_file(nullptr, nullptr),
          "Setting a history file without a watcher fails safely");
  size_t count = 7;
  require(hm_clipboard_snapshot(nullptr, &count) == nullptr && count == 0,
          "Snapshot without a watcher is empty");

  struct Watch {
    std::atomic<int> calls = 0;
  } watch;
  auto *clipboard = hm_clipboard_start(
      [](void *context) { ++static_cast<Watch *>(context)->calls; }, &watch, 3,
      nullptr);
  require(clipboard != nullptr, "The clipboard watcher starts");

  const auto wait_for_calls = [&watch](int wanted) {
    for (int i = 0; i < 1000 && watch.calls < wanted; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  };
  const auto find = [](const char *text, hm_clipboard_entry *entries,
                       size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (entries[i].text != nullptr && std::strcmp(entries[i].text, text) == 0)
        return entries[i].id;
    return uint64_t{0};
  };

  // The first pass seeds whatever is on the pasteboard, so the count starts at
  // 1 or more.
  const int seeded = watch.calls.load();
  const std::string one = "hm bridge test 2026-11-05 one";
  require(hm::write_pasteboard(
              {.kind = hm::ClipboardContent::Kind::text, .text = one})
              .has_value(),
          "Writing text to the pasteboard");
  wait_for_calls(seeded + 1);
  size_t entries = 0;
  auto *snapshot = hm_clipboard_snapshot(clipboard, &entries);
  require(snapshot != nullptr && entries >= 1, "A copy reaches the history");
  const auto one_id = find(one.c_str(), snapshot, entries);
  require(one_id != 0, "The copied text is in the history");
  hm_clipboard_free_snapshot(snapshot, entries);

  const std::string two = "hm bridge test 2026-11-05 two";
  require(hm::write_pasteboard(
              {.kind = hm::ClipboardContent::Kind::text, .text = two})
              .has_value(),
          "Writing the second text");
  wait_for_calls(seeded + 2);
  snapshot = hm_clipboard_snapshot(clipboard, &entries);
  require(snapshot != nullptr && entries >= 2, "Two copies are in the history");
  require(find(two.c_str(), snapshot, entries) != 0 &&
              std::strcmp(snapshot[0].text, two.c_str()) == 0,
          "The newest copy is first");
  hm_clipboard_free_snapshot(snapshot, entries);

  // Restoring puts the older entry back on the pasteboard without reordering
  // the history.
  require(hm_clipboard_restore(clipboard, one_id), "Restore finds the entry");
  const auto back = hm::read_pasteboard();
  require(back && back->kind == hm::ClipboardContent::Kind::text &&
              back->text == one,
          "The restored entry is on the pasteboard again");
  snapshot = hm_clipboard_snapshot(clipboard, &entries);
  require(snapshot != nullptr && snapshot[0].id != one_id,
          "Restore leaves the history order alone");
  hm_clipboard_free_snapshot(snapshot, entries);
  const int before_stop = watch.calls.load();
  require(before_stop == seeded + 2, "A restore does not notify");

  // Image entries carry their bytes across the bridge untouched.
  const std::vector<std::uint8_t> pixels{0x49, 0x49, 0x2A, 0x00, 1, 2, 3, 4, 5};
  require(hm::write_pasteboard(
              {.kind = hm::ClipboardContent::Kind::image, .bytes = pixels})
              .has_value(),
          "Writing an image to the pasteboard");
  wait_for_calls(seeded + 3);
  snapshot = hm_clipboard_snapshot(clipboard, &entries);
  require(snapshot != nullptr, "The image copy reaches the history");
  require(snapshot[0].kind == HM_CLIPBOARD_IMAGE &&
              snapshot[0].image != nullptr &&
              snapshot[0].image_size == pixels.size() &&
              std::memcmp(snapshot[0].image, pixels.data(), pixels.size()) == 0,
          "Image bytes cross the bridge untouched");
  hm_clipboard_free_snapshot(snapshot, entries);

  // The byte limit lands inside a four-byte UTF-8 character. The preview must
  // end before that character while restore retains the complete payload.
  const std::string prefix(hm::clipboard_preview_bytes - 1, 'x');
  const std::string long_text = prefix + "🙂tail";
  require(hm::write_pasteboard(
              {.kind = hm::ClipboardContent::Kind::text, .text = long_text})
              .has_value(),
          "Writing long Unicode text");
  wait_for_calls(seeded + 4);
  snapshot = hm_clipboard_snapshot(clipboard, &entries);
  require(snapshot != nullptr && snapshot[0].text != nullptr &&
              std::string(snapshot[0].text) == prefix,
          "The bridge preview never splits a Unicode character");
  const auto long_id = snapshot[0].id;
  hm_clipboard_free_snapshot(snapshot, entries);
  require(hm_clipboard_restore(clipboard, long_id),
          "A long clipboard entry restores");
  const auto complete = hm::read_pasteboard();
  require(complete && complete->text == long_text,
          "Preview truncation does not truncate the stored payload");

  hm_clipboard_clear(clipboard);
  require(hm_clipboard_snapshot(clipboard, &entries) == nullptr,
          "Clear empties the history");

  hm_clipboard_stop(clipboard);
  const int stopped_at = watch.calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  require(watch.calls.load() == stopped_at, "No callback after stop");
}
} // namespace

namespace {
struct DiskStatuses {
  std::mutex mutex;
  int scan_count = 0;
  bool scanning = false;
  std::string error;
};

void receive_disk_status(const hm_disk_status *status, void *context)
{
  auto *statuses = static_cast<DiskStatuses *>(context);
  const std::scoped_lock lock(statuses->mutex);
  statuses->scan_count = static_cast<int>(status->scan_count);
  statuses->scanning = status->scanning;
  statuses->error = status->error;
}

// Runs a real scan of a small folder. HOME moves too, so that history stays out
// of the real one.
void test_disk_tracker()
{
  namespace fs = std::filesystem;
  const auto base = fs::canonical(fs::temp_directory_path()) /
                    ("hm_bridge_test_" + std::to_string(getpid()));
  fs::remove_all(base);
  fs::create_directories(base / "scanned/project/.git");
  {
    std::ofstream file(base / "scanned/project/data", std::ios::binary);
    const std::string chunk(std::size_t{1024} * 1024, 'x');
    for (int i = 0; i < 120; ++i)
      file << chunk;
  }
  setenv("HOME", base.c_str(), 1);

  require(hm_disk_default_root_count() > 0, "Default disk roots");
  require(hm_disk_default_root(0)[0] == '/', "Default disk roots are absolute");

  hm_disk_tracker_set_roots(nullptr, nullptr, 0);
  hm_disk_tracker_scan(nullptr);
  hm_disk_tracker_stop(nullptr);
  hm_disk_report_free(nullptr);
  require(hm_disk_report_create(nullptr, 0) == nullptr,
          "A null tracker has no report");

  DiskStatuses statuses;
  const auto scanned = (base / "scanned").string();
  const char *roots[] = {scanned.c_str()};
  auto *tracker =
      hm_disk_tracker_start(receive_disk_status, &statuses, roots, 1);
  require(tracker != nullptr, "Disk tracker starts");
  require(hm_disk_volumes_json(nullptr) == nullptr,
          "Null tracker has no volume list");
  hm_disk_string_free(nullptr);
  hm_disk_scan_volume(nullptr, "missing");
  require(hm_disk_volume_report_create(nullptr, "", 0) == nullptr,
          "Null volume report is safe");
  char *volumes = hm_disk_volumes_json(tracker);
  require(volumes != nullptr &&
              std::strstr(volumes, "Internal disk") != nullptr,
          "Volume metadata crosses the bridge");
  hm_disk_string_free(volumes);
  char *added = hm_disk_add_root(tracker, scanned.c_str());
  require(added != nullptr && std::strstr(added, "error") == nullptr,
          "Folder enrollment reports success across the bridge");
  hm_disk_string_free(added);
  char *updated = hm_disk_set_volume_roots(tracker, "", roots, 1);
  require(updated != nullptr && std::strstr(updated, "error") == nullptr,
          "Root changes round trip across the bridge");
  hm_disk_string_free(updated);
  require(hm_disk_volume_report_create(tracker, "missing", 0) == nullptr,
          "Unknown drive has no report");

  require(hm_disk_report_create(tracker, 0) == nullptr,
          "No report before a scan");

  hm_disk_tracker_scan(tracker);
  bool done = false;
  for (int i = 0; i < 1000 && !done; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::scoped_lock lock(statuses.mutex);
    done = !statuses.scanning && statuses.scan_count == 1;
  }
  require(done, "The scan finishes");

  auto *report = hm_disk_report_create(tracker, 0);
  require(report != nullptr, "A scan gives a report");
  const auto summary = hm_disk_report_summary(report);
  require(!summary.has_baseline && !summary.volume_compared,
          "One scan has no baseline");
  require(summary.volume_total > 0 && summary.free_after > 0,
          "The volume is read");

  bool found_project = false;
  const auto project = (base / "scanned/project").string();
  for (std::size_t i = 0; i < hm_disk_report_row_count(report); ++i) {
    const auto row = hm_disk_report_row(report, i);
    if (project == row.path) {
      found_project = row.is_project && !row.is_file &&
                      row.after >= std::uint64_t{120} * 1024 * 1024;
      require(row.parent >= 0, "A nested row has a parent");
    }
  }
  require(found_project, "The report lists the project with its size");
  require(hm_disk_report_hotspot_count(report) == 0, "One scan blames nothing");
  require(hm_disk_report_unreadable_count(report) == 0,
          "Nothing was unreadable");
  hm_disk_report_free(report);

  hm_disk_tracker_stop(tracker);
  fs::remove_all(base);
}
} // namespace

int main()
{
  hm_sampler_stop(nullptr);
  hm_sampler_set_interval(nullptr, 1);

  Received received;
  auto *sampler = hm_sampler_start(receive, &received, 10);
  require(sampler != nullptr, "Sampler starts");
  hm_sampler_set_interval(sampler, 0.25);
  for (int i = 0; i < 500 && received.calls < 8; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  hm_sampler_stop(sampler);

  require(received.calls >= 2, "Callback repeats");
  const auto calls = received.calls.load();
  const auto &sample = received.last;
  require(std::isfinite(sample.cpu_percent), "CPU is known after two passes");
  require(sample.cpu_percent >= 0 && sample.cpu_percent <= 100,
          "CPU is a percentage");
  require(sample.core_count > 0, "Core count");
  require(std::isnan(sample.gpu_percent) ||
              (sample.gpu_percent >= 0 && sample.gpu_percent <= 100),
          "GPU is unknown or a percentage");
  require(sample.memory_total > 0 && sample.memory_used <= sample.memory_total,
          "Memory sizes");
  require(sample.memory_percent > 0 && sample.memory_percent <= 100,
          "Memory percent");
  require(sample.disk_percent > 0 && sample.disk_percent <= 100,
          "Disk percent");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  require(received.calls == calls, "No callback after stop");

  clipboard_round_trip();
  clipboard_persistence();
  // Last, because it moves HOME and the sampler above needs the real service.
  test_disk_tracker();
  return 0;
}
