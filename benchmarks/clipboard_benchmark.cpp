// Synthetic core workload: never reads or writes the system pasteboard.
#include "hm/clipboard.hpp"

#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

double cpu_seconds()
{
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    throw std::runtime_error("getrusage failed");
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
             1'000'000.0;
}

std::uint64_t resident_bytes()
{
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
    throw std::runtime_error("task_info failed");
  return info.resident_size;
}

void run(const std::filesystem::path &file, const char *name, std::size_t count,
         bool images, double seconds)
{
  hm::ClipboardHistory history(500);
  for (std::size_t i = 0; i < count; ++i) {
    hm::ClipboardContent content;
    if (images) {
      content.kind = hm::ClipboardContent::Kind::image;
      content.bytes.resize(std::size_t{96} * 1024,
                           static_cast<std::uint8_t>(i));
    } else {
      content.text = std::to_string(i) + std::string(1024, 'x');
    }
    history.capture(std::move(content), std::chrono::system_clock::now());
  }
  const auto payload_bytes = history.total_bytes();
  if (!hm::save_clipboard_history(file, history.entries()))
    throw std::runtime_error("initial save failed");
  history.clear();
  hm::PasteboardSources sources;
  sources.change_count = [] { return std::uint64_t{0}; };
  sources.read = [] {
    return hm::Collected<hm::ClipboardContent>{hm::ClipboardContent{}};
  };
  sources.write = [](const hm::ClipboardContent &) {
    return hm::Collected<void>{};
  };
  hm::ClipboardWatcher watcher({}, 500, sources, hm::clipboard_poll_interval,
                               file);
  // Warm up allocation and the poll thread before measuring idle residency.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const auto rss_start = resident_bytes();
  const auto cpu_start = cpu_seconds();
  const auto start = Clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  const auto elapsed =
      std::chrono::duration<double>(Clock::now() - start).count();
  const auto cpu = cpu_seconds() - cpu_start;
  const auto rss_end = resident_bytes();
  const auto snapshot_start = Clock::now();
  for (int i = 0; i < 10; ++i) {
    const auto snapshot = watcher.snapshot();
    if (snapshot.size() != count)
      throw std::runtime_error("snapshot count mismatch");
  }
  const auto snapshot_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - snapshot_start)
          .count() /
      10;
  const auto io_start = Clock::now();
  for (int i = 0; i < 3; ++i) {
    if (!hm::save_clipboard_history(file, watcher.snapshot()))
      throw std::runtime_error("save failed");
    const auto loaded = hm::load_clipboard_history(file);
    if (!loaded || loaded->size() != count)
      throw std::runtime_error("load count mismatch");
  }
  const auto roundtrip_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - io_start)
          .count() /
      3;
  std::cout << R"({"workload":")" << name << R"(","entries":)" << count
            << ",\"payload_bytes\":" << payload_bytes
            << ",\"idle_seconds\":" << elapsed
            << ",\"idle_cpu_percent_one_core\":" << cpu / elapsed * 100
            << ",\"rss_start_bytes\":" << rss_start
            << ",\"rss_end_bytes\":" << rss_end
            << ",\"snapshot_mean_ms\":" << snapshot_ms
            << ",\"save_load_mean_ms\":" << roundtrip_ms << "}\n"
            << std::flush;
}
} // namespace

int main(int argc, char **argv)
try {
  const double seconds = argc > 1 ? std::stod(argv[1]) : 120;
  if (!std::isfinite(seconds) || seconds < 1 || seconds > 3600)
    throw std::runtime_error(
        "idle duration must be 1 to 3600 seconds per workload");
  auto pattern =
      (std::filesystem::temp_directory_path() / "hm-benchmark-XXXXXX").string();
  if (mkdtemp(pattern.data()) == nullptr)
    throw std::runtime_error("cannot create private benchmark directory");
  const std::filesystem::path directory(pattern);
  try {
    const auto file = directory / "history.bin";
    run(file, "empty", 0, false, seconds);
    run(file, "500_text_1KiB", 500, false, seconds);
    run(file, "500_image_96KiB", 500, true, seconds);
  } catch (...) {
    std::filesystem::remove_all(directory);
    throw;
  }
  std::filesystem::remove_all(directory);
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
