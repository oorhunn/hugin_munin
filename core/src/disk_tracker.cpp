#include "hm/disk_tracker.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace hm {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view file_prefix = "snapshot-";
constexpr std::string_view file_suffix = ".hmdisk";

fs::path snapshot_path(const std::string &dir, std::int64_t time)
{
  return fs::path(dir) / (std::string(file_prefix) + std::to_string(time) +
                          std::string(file_suffix));
}

std::optional<std::int64_t> time_of_file(std::string_view name)
{
  if (!name.starts_with(file_prefix) || !name.ends_with(file_suffix))
    return std::nullopt;
  name.remove_prefix(file_prefix.size());
  name.remove_suffix(file_suffix.size());
  std::int64_t time = 0;
  const auto [end, error] =
      std::from_chars(name.data(), name.data() + name.size(), time);
  if (error != std::errc{} || end != name.data() + name.size())
    return std::nullopt;
  return time;
}

} // namespace

bool disk_scan_due(std::optional<std::int64_t> last_attempt,
                   std::chrono::seconds interval, std::int64_t now)
{
  return last_attempt && now - *last_attempt >= interval.count();
}

DiskTracker::DiskTracker(
    std::function<void(const DiskTrackerStatus &)> callback,
    DiskTrackerOptions options)
    : callback_(std::move(callback)), options_(std::move(options)),
      roots_(normalize_disk_roots(options_.roots))
{
  worker_ = std::jthread([this](const std::stop_token &stop) { run(stop); });
}

void DiskTracker::set_roots(std::vector<std::string> roots)
{
  const std::scoped_lock lock(mutex_);
  roots_ = normalize_disk_roots(std::move(roots));
}

void DiskTracker::scan_now()
{
  {
    const std::scoped_lock lock(mutex_);
    if (status_.scanning)
      return;
    scan_requested_ = true;
  }
  wakeup_.notify_all();
}

void DiskTracker::publish()
{
  DiskTrackerStatus status;
  {
    const std::scoped_lock lock(mutex_);
    status = status_;
  }
  callback_(status);
}

std::vector<std::int64_t> DiskTracker::scan_times() const
{
  std::vector<std::int64_t> times;
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(options_.history_dir, error))
    if (const auto time = time_of_file(entry.path().filename().string()))
      times.push_back(*time);
  std::ranges::sort(times);
  return times;
}

Collected<DiskSnapshot> DiskTracker::load(std::int64_t time) const
{
  std::ifstream file(snapshot_path(options_.history_dir, time),
                     std::ios::binary);
  if (!file)
    return std::unexpected("Could not open the disk snapshot from " +
                           std::to_string(time));
  std::ostringstream text;
  text << file.rdbuf();
  return parse_snapshot(text.str());
}

Collected<void> DiskTracker::save(const DiskSnapshot &snapshot) const
{
  std::error_code error;
  fs::create_directories(options_.history_dir, error);
  if (error)
    return std::unexpected("Could not create the disk history folder: " +
                           error.message());

  // Written beside its final name and renamed, so that a full disk or a crash
  // cannot leave a damaged snapshot behind.
  const auto path = snapshot_path(options_.history_dir, snapshot.time);
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << serialize_snapshot(snapshot);
    file.flush();
    if (!file) {
      file.close();
      fs::remove(temporary, error);
      return std::unexpected("Could not write the disk snapshot");
    }
  }
  fs::rename(temporary, path, error);
  if (error) {
    const auto message = error.message();
    fs::remove(temporary, error);
    return std::unexpected("Could not save the disk snapshot: " + message);
  }
  return {};
}

void DiskTracker::refresh_history_status()
{
  std::vector<std::int64_t> times;
  std::size_t unreadable_count = 0;
  {
    const std::scoped_lock lock(history_mutex_);
    times = scan_times();
    if (!times.empty())
      if (const auto latest = load(times.back()))
        unreadable_count = latest->unreadable.size();
  }
  const std::scoped_lock lock(mutex_);
  status_.scan_count = times.size();
  status_.unreadable_count = unreadable_count;
  status_.last_scan =
      times.empty() ? std::nullopt : std::optional(times.back());
  // Only ever moves forward: with no scan saved yet, a failed first attempt
  // must still count, or the schedule would never start.
  if (!times.empty() && (!last_attempt_ || *last_attempt_ < times.back()))
    last_attempt_ = times.back();
}

void DiskTracker::scan(std::vector<std::string> roots,
                       const std::stop_token &stop)
{
  {
    const std::scoped_lock lock(mutex_);
    status_.scanning = true;
    status_.files_seen = status_.bytes_seen = 0;
    status_.error.clear();
    // Set now rather than after the save, so that a failed save, likely on a
    // full disk, waits for the next interval instead of starting over at once.
    last_attempt_ = options_.now();
  }
  publish();

  DiskSnapshotBuilder builder([this](std::uint64_t files, std::uint64_t bytes) {
    {
      const std::scoped_lock lock(mutex_);
      status_.files_seen = files;
      status_.bytes_seen = bytes;
    }
    publish();
  });
  options_.walk(roots, builder, stop);

  std::string error;
  if (!stop.stop_requested()) {
    auto snapshot = builder.finish();
    snapshot.time = options_.now();
    snapshot.roots = std::move(roots);
    if (const auto disk = options_.disk()) {
      snapshot.volume_total = disk->total;
      snapshot.volume_free = disk->total - disk->used;
    }
    const std::scoped_lock lock(history_mutex_);
    if (const auto saved = save(snapshot); !saved)
      error = saved.error();
    std::error_code ignored;
    for (const auto time : expired_scans(scan_times(), snapshot.time))
      fs::remove(snapshot_path(options_.history_dir, time), ignored);
  }

  refresh_history_status();
  {
    const std::scoped_lock lock(mutex_);
    status_.scanning = false;
    status_.error = std::move(error);
  }
  publish();
}

void DiskTracker::run(const std::stop_token &stop)
{
  prepare_disk_scan_thread();
  refresh_history_status();
  publish();

  while (!stop.stop_requested()) {
    std::unique_lock lock(mutex_);
    auto wait = options_.max_wait;
    if (last_attempt_) {
      const auto remaining =
          std::chrono::seconds(*last_attempt_ - options_.now()) +
          options_.scan_interval;
      wait = std::clamp(remaining, std::chrono::seconds(1), options_.max_wait);
    }
    wakeup_.wait_for(lock, stop, wait, [this] { return scan_requested_; });
    if (stop.stop_requested())
      return;
    if (!scan_requested_ &&
        !disk_scan_due(last_attempt_, options_.scan_interval, options_.now()))
      continue;
    scan_requested_ = false;
    auto roots = roots_;
    lock.unlock();
    scan(std::move(roots), stop);
  }
}

Collected<DiskReport> DiskTracker::report(std::int64_t since_seconds,
                                          const DiskReportLimits &limits)
{
  const std::scoped_lock lock(history_mutex_);
  const auto times = scan_times();
  if (times.empty())
    return std::unexpected("No scans yet");

  const auto latest = load(times.back());
  if (!latest)
    return std::unexpected(latest.error());
  const auto baseline_time = choose_baseline(times, since_seconds);
  if (!baseline_time)
    return compare_snapshots(nullptr, *latest, limits);
  const auto baseline = load(*baseline_time);
  if (!baseline)
    return std::unexpected(baseline.error());
  return compare_snapshots(&*baseline, *latest, limits);
}

} // namespace hm
