// The C interface the Swift UI uses, against the live system.
#include "check.hpp"
#include "hm/pasteboard.hpp"
#include "hm_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

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

  hm_clipboard_clear(clipboard);
  require(hm_clipboard_snapshot(clipboard, &entries) == nullptr,
          "Clear empties the history");

  hm_clipboard_stop(clipboard);
  const int stopped_at = watch.calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  require(watch.calls.load() == stopped_at, "No callback after stop");
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
  for (int i = 0; i < 500 && received.calls < 2; ++i)
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
  return 0;
}
