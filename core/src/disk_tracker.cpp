#include "hm/disk_tracker.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>

namespace hm {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::string_view file_prefix = "snapshot-", file_suffix = ".hmdisk";

bool under(const std::string &path, const std::string &root)
{
  return path == root ||
         (path.starts_with(root) && (root == "/" || path[root.size()] == '/'));
}
std::string at_mount(const std::string &mount, const std::string &path)
{
  return mount == "/" ? path : mount + (path == "/" ? "" : path);
}
std::string relative_path(const std::string &path, const std::string &mount)
{
  if (mount == "/")
    return path;
  return path == mount ? "/" : path.substr(mount.size());
}
std::string encoded_id(const std::string &id)
{
  constexpr std::string_view hex = "0123456789abcdef";
  std::string out;
  for (const unsigned char byte : id) {
    out += hex[byte >> 4U];
    out += hex[byte & 15U];
  }
  return out;
}
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
  return error == std::errc{} && end == name.data() + name.size()
             ? std::optional(time)
             : std::nullopt;
}
Collected<void> atomic_write(const fs::path &path, const std::string &text)
{
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error)
    return std::unexpected("Could not create the disk history folder: " +
                           error.message());
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << text;
    file.flush();
    if (!file) {
      file.close();
      fs::remove(temporary, error);
      return std::unexpected("Could not write the disk history");
    }
  }
  fs::rename(temporary, path, error);
  if (error) {
    const auto message = error.message();
    fs::remove(temporary, error);
    return std::unexpected("Could not save the disk history: " + message);
  }
  return {};
}
bool valid_relative(const std::string &path)
{
  return !path.empty() && path.front() == '/' &&
         fs::path(path).lexically_normal().string() == path &&
         !path.contains("/../") && !path.ends_with("/..");
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
    : callback_(std::move(callback)), options_(std::move(options))
{
  Target internal;
  internal.volume.name = "Internal disk";
  internal.volume.mount_path = "/";
  internal.available = true;
  targets_.emplace("", std::move(internal));
  load_targets();
  // Legacy external roots are enrolled only if their identity can be resolved
  // now. Unresolved /Volumes roots remain visible in preferences but are never
  // scanned as internal.
  set_roots(options_.roots);
  worker_ = std::jthread([this](const std::stop_token &stop) { run(stop); });
}

void DiskTracker::set_roots(const std::vector<std::string> &roots)
{
  std::vector<std::string> internal;
  for (const auto &input : roots) {
    const auto normalized = normalize_disk_roots({input});
    if (normalized.empty())
      continue;
    const auto &root = normalized.front();
    const auto volume = options_.volume_for_path(root);
    if (volume && !volume->id.empty()) {
      const auto added = add_root(root);
      if (!added) {
        const std::scoped_lock lock(mutex_);
        configuration_error_ = added.error();
      }
    } else if (under(root, "/Volumes")) {
      const std::scoped_lock lock(mutex_);
      configuration_error_ =
          "Saved external folder could not be identified: " + root +
          ". Reconnect its drive and add it again.";
    } else {
      internal.push_back(root);
    }
  }
  const std::scoped_lock lock(mutex_);
  targets_.at("").roots = normalize_disk_roots(std::move(internal));
}

Collected<std::string> DiskTracker::add_root(const std::string &path)
{
  std::error_code error;
  const auto real = fs::canonical(path, error);
  const auto normalized = normalize_disk_roots(
      {error ? fs::path(path).lexically_normal().string() : real.string()});
  if (normalized.size() != 1)
    return std::unexpected("Choose an absolute folder path");
  const auto &root = normalized.front();
  const auto volume = options_.volume_for_path(root);
  if (!volume)
    return std::unexpected(volume.error());
  if (!under(root, volume->mount_path))
    return std::unexpected("The folder does not belong to this drive");
  if (!volume->id.empty()) {
    const auto mounted = options_.volumes();
    if (!mounted ||
        std::ranges::count(*mounted, volume->id, &DiskVolume::id) != 1)
      return std::unexpected("This drive's identity is unavailable or "
                             "duplicated. Disconnect cloned media and retry.");
  }

  const std::scoped_lock lock(mutex_);
  const bool existed = targets_.contains(volume->id);
  auto &target = targets_[volume->id];
  const auto previous = target;
  target.volume = *volume;
  target.available = true;
  target.roots.push_back(
      volume->id.empty() ? root : relative_path(root, volume->mount_path));
  target.roots = normalize_disk_roots(std::move(target.roots));
  if (const auto saved = save_targets(); !saved) {
    target = previous;
    if (!existed)
      targets_.erase(volume->id);
    return std::unexpected(saved.error());
  }
  wakeup_.notify_all();
  return volume->id;
}

Collected<void>
DiskTracker::set_volume_roots(const std::string &id,
                              const std::vector<std::string> &roots)
{
  const std::scoped_lock lock(mutex_);
  const auto found = targets_.find(id);
  if (found == targets_.end())
    return std::unexpected("This drive is not tracked");
  auto &target = found->second;
  auto previous = target.roots;
  std::vector<std::string> relative;
  for (const auto &input : roots) {
    std::error_code error;
    auto path = fs::canonical(input, error);
    if (error)
      path = fs::path(input).lexically_normal();
    const auto root = path.string();
    if (!under(root, target.volume.mount_path) || !valid_relative(root))
      return std::unexpected("Choose a folder on the selected drive");
    const auto actual = options_.volume_for_path(root);
    if ((actual && actual->id != id) || (id.empty() && under(root, "/Volumes")))
      return std::unexpected("Add this folder with Add Drive or Folder so its "
                             "volume is tracked separately");
    relative.push_back(relative_path(root, target.volume.mount_path));
  }
  target.roots = normalize_disk_roots(std::move(relative));
  if (const auto saved = save_targets(); !saved) {
    target.roots = std::move(previous);
    return saved;
  }
  return {};
}

Collected<void> DiskTracker::forget_volume(const std::string &id)
{
  const std::scoped_lock lock(mutex_);
  if (id.empty())
    return std::unexpected("The internal disk cannot be removed");
  const auto found = targets_.find(id);
  if (found == targets_.end())
    return {};
  if (found->second.status.scanning)
    return std::unexpected("Wait for this drive's scan to finish");
  const auto previous = found->second;
  targets_.erase(found);
  if (const auto saved = save_targets(); !saved) {
    targets_.emplace(id, previous);
    return saved;
  }
  requested_.erase(id);
  return {};
}

void DiskTracker::scan_now(const std::string &volume_id)
{
  {
    const std::scoped_lock lock(mutex_);
    const auto found = targets_.find(volume_id);
    if (found == targets_.end() || found->second.status.scanning)
      return;
    auto &target = found->second;
    const bool was_active = target.active;
    target.active = true;
    if (!volume_id.empty()) {
      if (const auto saved = save_targets(); !saved) {
        target.status.error = saved.error();
        target.active = was_active;
        return;
      }
    }
    requested_.insert(volume_id);
  }
  wakeup_.notify_all();
}

std::vector<DiskVolumeStatus> DiskTracker::volumes() const
{
  const std::scoped_lock lock(mutex_);
  std::vector<DiskVolumeStatus> result;
  for (const auto &[id, target] : targets_) {
    auto roots = target.roots;
    if (!id.empty())
      for (auto &root : roots)
        root = at_mount(target.volume.mount_path, root);
    auto status = target.status;
    if (id.empty() && !configuration_error_.empty())
      status.error = configuration_error_;
    result.push_back({target.volume, true, target.available, std::move(roots),
                      std::move(status)});
  }
  for (const auto &volume : mounted_)
    if (!targets_.contains(volume.id))
      result.push_back({volume, false, true, {}, {}});
  return result;
}

void DiskTracker::publish()
{
  DiskTrackerStatus status;
  {
    const std::scoped_lock lock(mutex_);
    status = targets_.at("").status;
    if (!configuration_error_.empty())
      status.error = configuration_error_;
  }
  callback_(status);
}

std::string DiskTracker::history_dir(const std::string &id) const
{
  return id.empty()
             ? options_.history_dir
             : (fs::path(options_.history_dir) / "volumes" / encoded_id(id))
                   .string();
}
std::vector<std::int64_t> DiskTracker::scan_times(const std::string &id) const
{
  std::vector<std::int64_t> times;
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(history_dir(id), error))
    if (const auto time = time_of_file(entry.path().filename().string()))
      times.push_back(*time);
  std::ranges::sort(times);
  return times;
}
Collected<DiskSnapshot> DiskTracker::load(const std::string &id,
                                          std::int64_t time) const
{
  std::ifstream file(snapshot_path(history_dir(id), time), std::ios::binary);
  if (!file)
    return std::unexpected("Could not open the disk snapshot from " +
                           std::to_string(time));
  std::ostringstream text;
  text << file.rdbuf();
  auto snapshot = parse_snapshot(text.str());
  if (snapshot && snapshot->volume_id != id)
    return std::unexpected("Disk snapshot belongs to a different volume");
  return snapshot;
}
Collected<void> DiskTracker::save(const std::string &id,
                                  const DiskSnapshot &snapshot) const
{
  return atomic_write(snapshot_path(history_dir(id), snapshot.time),
                      serialize_snapshot(snapshot));
}

Collected<void> DiskTracker::save_targets() const
{
  if (configuration_error_.starts_with("Could not read tracked drives"))
    return std::unexpected(configuration_error_);
  Json list = Json::array();
  for (const auto &[id, target] : targets_)
    if (!id.empty())
      list.push_back({{"id", id},
                      {"name", target.volume.name},
                      {"mount", target.volume.mount_path},
                      {"roots", target.roots},
                      {"active", target.active}});
  return atomic_write(fs::path(options_.history_dir) / "volumes.json",
                      Json{{"version", 1}, {"volumes", list}}.dump(2));
}
void DiskTracker::load_targets()
{
  const auto path = fs::path(options_.history_dir) / "volumes.json";
  std::ifstream file(path);
  if (!file)
    return;
  try {
    const auto config = Json::parse(file);
    if (config.at("version") != 1)
      throw std::runtime_error("Unknown version");
    std::map<std::string, Target> loaded;
    for (const auto &item : config.at("volumes")) {
      Target target;
      target.volume.id = item.at("id").get<std::string>();
      target.volume.name = item.at("name").get<std::string>();
      target.volume.mount_path = item.at("mount").get<std::string>();
      target.roots = item.at("roots").get<std::vector<std::string>>();
      target.active = item.at("active").get<bool>();
      if (target.volume.id.empty() ||
          !valid_relative(target.volume.mount_path) ||
          !std::ranges::all_of(target.roots, valid_relative))
        throw std::runtime_error("Invalid tracked volume");
      const auto id = target.volume.id;
      if (!loaded.emplace(id, std::move(target)).second)
        throw std::runtime_error("Duplicate volume identity");
    }
    targets_.merge(loaded);
  } catch (const std::exception &) {
    configuration_error_ = "Could not read tracked drives. Restore "
                           "volumes.json before changing drive tracking.";
  }
}

void DiskTracker::reconcile()
{
  const auto mounted = options_.volumes();
  const std::scoped_lock lock(mutex_);
  mounted_.clear();
  if (mounted)
    mounted_ = *mounted;
  bool metadata_changed = false;
  for (auto &[id, target] : targets_) {
    if (id.empty())
      continue;
    const auto count = std::ranges::count(mounted_, id, &DiskVolume::id);
    const bool was_available = target.available;
    target.available = count == 1;
    if (!was_available && target.available)
      target.last_attempt = target.status.last_scan;
    if (target.available) {
      const auto &volume = *std::ranges::find(mounted_, id, &DiskVolume::id);
      metadata_changed = metadata_changed ||
                         target.volume.mount_path != volume.mount_path ||
                         target.volume.name != volume.name;
      target.volume = volume;
    }
  }
  if (metadata_changed)
    if (const auto saved = save_targets(); !saved)
      configuration_error_ = saved.error();
  // Duplicate UUIDs (for example cloned media) must not be auto-associated.
  std::erase_if(mounted_, [&](const auto &volume) {
    return volume.id.empty() ||
           std::ranges::count(*mounted, volume.id, &DiskVolume::id) != 1;
  });
}

void DiskTracker::refresh_history_status(const std::string &id)
{
  std::vector<std::int64_t> times;
  std::size_t unreadable_count = 0;
  {
    const std::scoped_lock lock(history_mutex_);
    times = scan_times(id);
    if (!times.empty())
      if (const auto latest = load(id, times.back()))
        unreadable_count = latest->unreadable.size();
  }
  const std::scoped_lock lock(mutex_);
  const auto found = targets_.find(id);
  if (found == targets_.end())
    return;
  auto &target = found->second;
  target.status.scan_count = times.size();
  target.status.unreadable_count = unreadable_count;
  target.status.last_scan =
      times.empty() ? std::nullopt : std::optional(times.back());
  if (!times.empty()) {
    target.active = true;
    if (!target.last_attempt || *target.last_attempt < times.back())
      target.last_attempt = times.back();
  }
}

void DiskTracker::scan(const std::string &id, const std::stop_token &stop)
{
  Target target;
  {
    const std::scoped_lock lock(mutex_);
    const auto found = targets_.find(id);
    if (found == targets_.end())
      return;
    auto &current = found->second;
    current.last_attempt = options_.now();
    if (!current.available || current.roots.empty()) {
      current.status.error =
          current.roots.empty()
              ? "Add a folder before scanning"
              : "Drive unavailable. Connect or unlock it to scan.";
      return;
    }
    current.status.scanning = true;
    current.status.files_seen = current.status.bytes_seen = 0;
    current.status.error.clear();
    target = current;
  }
  publish();

  std::vector<std::string> roots;
  roots.reserve(target.roots.size());
  for (const auto &root : target.roots)
    roots.push_back(at_mount(target.volume.mount_path, root));
  const auto validate = [&]() -> std::optional<DiskVolume> {
    if (id.empty())
      return target.volume;
    const auto mounted = options_.volumes();
    if (!mounted || std::ranges::count(*mounted, id, &DiskVolume::id) != 1)
      return std::nullopt;
    const auto &volume = *std::ranges::find(*mounted, id, &DiskVolume::id);
    if (volume.mount_path != target.volume.mount_path ||
        volume.session != target.volume.session)
      return std::nullopt;
    for (const auto &root : roots) {
      const auto actual = options_.volume_for_path(root);
      if (actual && actual->id != id)
        return std::nullopt;
    }
    return volume;
  };
  DiskSnapshotBuilder builder(
      [this, &id](std::uint64_t files, std::uint64_t bytes) {
        {
          const std::scoped_lock lock(mutex_);
          auto &status = targets_.at(id).status;
          status.files_seen = files;
          status.bytes_seen = bytes;
        }
        publish();
      });
  std::string error;
  if (!validate())
    error = "Drive changed or disconnected. Previous scan retained.";
  else
    options_.walk(roots, builder, stop);

  if (!stop.stop_requested() && error.empty()) {
    auto snapshot = builder.finish();
    const auto final_volume = validate();
    if (!final_volume) {
      error = "Drive changed or disconnected during the scan. Previous scan "
              "retained.";
    } else if (!id.empty() &&
               std::ranges::any_of(snapshot.unreadable, [&](const auto &path) {
                 return std::ranges::find(roots, path) != roots.end();
               })) {
      error = "A selected folder could not be read. Allow folder access and "
              "retry. Previous scan retained.";
    } else {
      snapshot.time = options_.now();
      snapshot.volume_id = id;
      snapshot.roots = target.roots;
      if (id.empty()) {
        if (const auto disk = options_.disk()) {
          snapshot.volume_total = disk->total;
          snapshot.volume_free = disk->total - disk->used;
        }
      } else {
        snapshot.volume_total = final_volume->total;
        snapshot.volume_free = final_volume->free;
        for (auto &entry : snapshot.entries)
          entry.path = relative_path(entry.path, target.volume.mount_path);
        for (auto &path : snapshot.unreadable)
          path = relative_path(path, target.volume.mount_path);
      }
      const std::scoped_lock lock(history_mutex_);
      if (const auto saved = save(id, snapshot); !saved)
        error = saved.error();
      else {
        std::error_code ignored;
        for (const auto time : expired_scans(scan_times(id), snapshot.time))
          fs::remove(snapshot_path(history_dir(id), time), ignored);
      }
    }
  }
  refresh_history_status(id);
  {
    const std::scoped_lock lock(mutex_);
    auto &status = targets_.at(id).status;
    status.scanning = false;
    status.error = std::move(error);
  }
  publish();
}

void DiskTracker::run(const std::stop_token &stop)
{
  prepare_disk_scan_thread();
  for (const auto &volume : volumes())
    refresh_history_status(volume.volume.id);
  reconcile();
  publish();
  while (!stop.stop_requested()) {
    std::unique_lock lock(mutex_);
    // Polling also catches mount changes and wake; no drive is enrolled by
    // discovery.
    wakeup_.wait_for(lock, stop,
                     std::min(options_.max_wait, std::chrono::seconds(15)),
                     [this] { return !requested_.empty(); });
    if (stop.stop_requested())
      return;
    lock.unlock();
    reconcile();
    lock.lock();
    auto pending = std::move(requested_);
    requested_.clear();
    for (const auto &[id, target] : targets_)
      if (target.active && target.available &&
          (!target.last_attempt ||
           disk_scan_due(target.last_attempt, options_.scan_interval,
                         options_.now())))
        pending.insert(id);
    lock.unlock();
    for (const auto &id : pending) {
      if (stop.stop_requested())
        return;
      refresh_history_status(id);
      scan(id, stop);
    }
    publish();
  }
}

Collected<DiskReport> DiskTracker::report(std::int64_t since_seconds,
                                          const DiskReportLimits &limits,
                                          const std::string &volume_id)
{
  std::string mount;
  {
    const std::scoped_lock lock(mutex_);
    const auto found = targets_.find(volume_id);
    if (found == targets_.end())
      return std::unexpected("This drive is not tracked");
    mount = found->second.volume.mount_path;
  }
  const std::scoped_lock lock(history_mutex_);
  const auto times = scan_times(volume_id);
  if (times.empty())
    return std::unexpected("No scans yet");
  const auto latest = load(volume_id, times.back());
  if (!latest)
    return std::unexpected(latest.error());
  const auto baseline_time = choose_baseline(times, since_seconds);
  std::optional<DiskSnapshot> baseline;
  if (baseline_time) {
    auto loaded = load(volume_id, *baseline_time);
    if (!loaded)
      return std::unexpected(loaded.error());
    baseline = std::move(*loaded);
  }
  auto result =
      compare_snapshots(baseline ? &*baseline : nullptr, *latest, limits);
  if (!volume_id.empty()) {
    for (auto &row : result.rows)
      row.path = at_mount(mount, row.path);
    for (auto &path : result.unreadable)
      path = at_mount(mount, path);
  }
  return result;
}
} // namespace hm
