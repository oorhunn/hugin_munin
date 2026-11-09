#include "hm/coordinator.hpp"
#include "hm_bridge.h"

#include "hm/clipboard.hpp"
#include "hm/file_shelf.hpp"
#include "hm/sampler.hpp"
#include <cstdlib>
#include <cstring>

#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

struct hm_sampler {
  std::mutex mutex;
  std::condition_variable_any wake;
  double interval;
  bool changed = false;
  std::jthread worker;
  hm_sampler(hm_sample_callback callback, void *context, double seconds)
      : interval(seconds),
        worker([this, callback, context](const std::stop_token &stop) {
          while (!stop.stop_requested()) {
            hm_sample out{};
            out.cpu_percent = out.memory_percent = out.disk_percent =
                out.gpu_percent = std::numeric_limits<double>::quiet_NaN();
            std::string snapshot;
            try {
              hm::ensure_coordinator();
              auto result =
                  hm::coordinator_request({{"tool", "get_capacity"},
                                           {"arguments", hm::Json::object()}});
              snapshot = result.dump();
              auto &s = result.at("system");
              auto percent = [&](const char *key) {
                return s.contains(key) && s[key].is_number()
                           ? s[key].get<double>()
                           : std::numeric_limits<double>::quiet_NaN();
              };
              out.cpu_percent = percent("cpu_percent");
              out.memory_percent = percent("memory_percent");
              out.disk_percent = percent("disk_percent");
              out.gpu_percent = percent("gpu_percent");
              out.core_count = s.value("core_count", size_t{0});
              out.gpu_core_count = s.value("gpu_core_count", size_t{0});
              out.memory_used = s.value("memory_used", uint64_t{0});
              out.memory_total = s.value("memory_total", uint64_t{0});
              out.disk_free = s.value("disk_free", uint64_t{0});
            } catch (const std::exception &) {
              snapshot =
                  R"({"error":"Resource service unavailable","jobs":[]})";
            }
            out.coordination_json = snapshot.c_str();
            callback(&out, context);
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::duration<double>(interval),
                          [this] { return changed; });
            changed = false;
          }
        })
  {
  }
};
struct hm_clipboard {
  hm::ClipboardWatcher watcher;
};

namespace {
// Cuts a UTF-8 preview of at most clipboard_preview_bytes bytes, never
// splitting a character.
std::string preview(const std::string &text)
{
  if (text.size() <= hm::clipboard_preview_bytes)
    return text;
  std::size_t end = hm::clipboard_preview_bytes;
  // A continuation byte is 10xxxxxx; back up to the start of the character.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
    --end;
  return text.substr(0, end);
}

// Returns a malloc'd copy, or nullptr when out of memory; the caller checks.
char *duplicate(const char *bytes, std::size_t count)
{
  auto *copy = static_cast<char *>(std::malloc(count));
  if (copy != nullptr)
    std::memcpy(copy, bytes, count);
  return copy;
}
} // namespace

extern "C" {
int hm_admissions_set_paused(int paused)
{
  try {
    hm::coordinator_request({{"tool", "set_admissions_paused"},
                             {"arguments", {{"paused", paused != 0}}}});
    return 1;
  } catch (...) {
    return 0;
  }
}
hm_sampler *hm_sampler_start(hm_sample_callback callback, void *context,
                             double interval_seconds)
{
  if (!callback)
    return nullptr;
  try {
    return new hm_sampler(callback, context,
                          hm::clamp_sample_interval(
                              std::chrono::duration<double>(interval_seconds))
                              .count());
  } catch (...) {
    return nullptr;
  }
}
void hm_sampler_set_interval(hm_sampler *sampler, double interval_seconds)
{
  if (!sampler)
    return;
  {
    const std::scoped_lock guard(sampler->mutex);
    sampler->interval = hm::clamp_sample_interval(
                            std::chrono::duration<double>(interval_seconds))
                            .count();
    sampler->changed = true;
  }
  sampler->wake.notify_all();
}
void hm_sampler_stop(hm_sampler *sampler)
{
  delete sampler;
}
hm_clipboard *hm_clipboard_start(hm_clipboard_callback callback, void *context,
                                 size_t capacity, const char *history_file)
{
  try {
    return new hm_clipboard{hm::ClipboardWatcher(
        [callback, context] { callback(context); }, capacity, {},
        hm::clipboard_poll_interval,
        history_file != nullptr ? std::filesystem::path(history_file)
                                : std::filesystem::path())};
  } catch (const std::exception &) {
    return nullptr;
  }
}

bool hm_clipboard_set_history_file(hm_clipboard *clipboard,
                                   const char *history_file)
{
  if (clipboard == nullptr)
    return false;
  try {
    clipboard->watcher.set_history_file(
        history_file != nullptr ? std::filesystem::path(history_file)
                                : std::filesystem::path());
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void hm_clipboard_stop(hm_clipboard *clipboard)
{
  delete clipboard;
}

void hm_clipboard_set_capacity(hm_clipboard *clipboard, size_t capacity)
{
  if (clipboard)
    clipboard->watcher.set_capacity(capacity);
}

hm_clipboard_entry *hm_clipboard_snapshot(hm_clipboard *clipboard,
                                          size_t *count)
{
  if (count == nullptr)
    return nullptr;
  *count = 0;
  if (clipboard == nullptr)
    return nullptr;
  try {
    const auto entries = clipboard->watcher.snapshot();
    if (entries.empty())
      return nullptr;
    auto *out = static_cast<hm_clipboard_entry *>(
        std::calloc(entries.size(), sizeof(hm_clipboard_entry)));
    if (out == nullptr)
      return nullptr;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto &entry = entries[i];
      const auto seconds =
          std::chrono::duration<double>(entry.captured_at.time_since_epoch());
      out[i].id = entry.id;
      out[i].kind = entry.content.kind == hm::ClipboardContent::Kind::image
                        ? HM_CLIPBOARD_IMAGE
                        : HM_CLIPBOARD_TEXT;
      out[i].captured_at = seconds.count();
      out[i].size = entry.content.size();
      if (entry.content.kind == hm::ClipboardContent::Kind::text) {
        const auto text = preview(entry.content.text);
        // One byte extra for the terminator; failure leaves the field NULL.
        out[i].text = duplicate(text.c_str(), text.size() + 1);
      } else {
        out[i].image_size = entry.content.bytes.size();
        out[i].image = reinterpret_cast<const std::uint8_t *>(duplicate(
            reinterpret_cast<const char *>(entry.content.bytes.data()),
            entry.content.bytes.size()));
      }
    }
    *count = entries.size();
    return out;
  } catch (const std::exception &) {
    return nullptr;
  }
}

void hm_clipboard_free_snapshot(hm_clipboard_entry *entries, size_t count)
{
  if (entries == nullptr)
    return;
  for (std::size_t i = 0; i < count; ++i) {
    std::free(const_cast<char *>(entries[i].text));
    std::free(const_cast<std::uint8_t *>(entries[i].image));
  }
  std::free(entries);
}

bool hm_clipboard_restore(hm_clipboard *clipboard, uint64_t id)
{
  return clipboard != nullptr && clipboard->watcher.restore(id);
}

size_t hm_clipboard_stack_position(const hm_clipboard *clipboard)
{
  return clipboard != nullptr ? clipboard->watcher.stack_position() : 0;
}

size_t hm_clipboard_stack_move(hm_clipboard *clipboard, hm_clipboard_step step)
{
  return clipboard != nullptr ? clipboard->watcher.stack_move(
                                    static_cast<hm::ClipboardStep>(step))
                              : 0;
}

size_t hm_clipboard_stack_jump(hm_clipboard *clipboard, size_t position)
{
  return clipboard != nullptr ? clipboard->watcher.stack_jump(position) : 0;
}

bool hm_clipboard_stack_restore(hm_clipboard *clipboard)
{
  return clipboard != nullptr && clipboard->watcher.stack_restore();
}

void hm_clipboard_stack_remove(hm_clipboard *clipboard)
{
  if (clipboard)
    clipboard->watcher.stack_remove();
}

void hm_clipboard_clear(hm_clipboard *clipboard)
{
  if (clipboard)
    clipboard->watcher.clear();
}
}

struct hm_file_shelf {
  hm::FileShelf value;
};

extern "C" {
hm_file_shelf *hm_file_shelf_create(void)
{
  try {
    return new hm_file_shelf;
  } catch (...) {
    return nullptr;
  }
}
void hm_file_shelf_destroy(hm_file_shelf *shelf)
{
  delete shelf;
}
int hm_file_shelf_add(hm_file_shelf *shelf, const char *path)
{
  try {
    return shelf && path && shelf->value.add(path);
  } catch (...) {
    return 0;
  }
}
void hm_file_shelf_remove(hm_file_shelf *shelf, const char *path)
{
  if (shelf && path)
    shelf->value.remove(path);
}
size_t hm_file_shelf_count(const hm_file_shelf *shelf)
{
  return shelf ? shelf->value.paths().size() : 0;
}
const char *hm_file_shelf_path(const hm_file_shelf *shelf, size_t index)
{
  return shelf && index < shelf->value.paths().size()
             ? shelf->value.paths()[index].c_str()
             : nullptr;
}
}
