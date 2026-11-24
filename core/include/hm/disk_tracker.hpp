// Scans the disk on a schedule and keeps the results, so that two points in
// time can be compared.
#pragma once

#include "hm/collectors.hpp"
#include "hm/disk_snapshot.hpp"
#include "hm/disk_volume.hpp"
#include "hm/disk_walk.hpp"
#include <map>
#include <set>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace hm {

struct DiskTrackerStatus {
  bool scanning = false;
  std::uint64_t files_seen = 0, bytes_seen = 0; // progress of the running scan
  std::optional<std::int64_t> last_scan;
  std::size_t scan_count = 0;
  std::size_t unreadable_count = 0; // in the latest scan
  std::string error; // why the latest scan could not be saved, if it could not
};

struct DiskVolumeStatus {
  DiskVolume volume;
  bool tracked = false, available = false;
  std::vector<std::string> roots; // absolute display paths
  DiskTrackerStatus status;
};

struct DiskTrackerOptions {
  std::string history_dir = default_disk_history_dir();
  std::vector<std::string> roots = default_disk_roots();
  std::chrono::seconds scan_interval = std::chrono::hours(6);
  // How long the worker sleeps at most, so that a wait outlasting a system
  // sleep still ends.
  std::chrono::seconds max_wait = std::chrono::minutes(15);
  std::function<void(const std::vector<std::string> &, DiskWalkVisitor &,
                     const std::stop_token &)>
      walk = walk_disk;
  std::function<Collected<Disk>()> disk = collect_disk;
  std::function<Collected<std::vector<DiskVolume>>()> volumes =
      mounted_disk_volumes;
  std::function<Collected<DiskVolume>(const std::string &)> volume_for_path =
      disk_volume_for_path;
  std::function<std::int64_t()> now = [] {
    return static_cast<std::int64_t>(std::time(nullptr));
  };
};

// True when a scan is overdue. Nothing scans by itself until the first scan has
// been asked for, because walking the home folder raises the system's privacy
// prompts.
bool disk_scan_due(std::optional<std::int64_t> last_attempt,
                   std::chrono::seconds interval, std::int64_t now);

// Calls `callback` on a background thread straight away and whenever the status
// changes. Destroying the tracker stops a running scan and the thread, so it
// must not be destroyed from inside `callback`.
class DiskTracker {
public:
  explicit DiskTracker(std::function<void(const DiskTrackerStatus &)> callback,
                       DiskTrackerOptions options = {});

  // Used from the next scan on. A report only compares folders that both of its
  // scans covered.
  void set_roots(const std::vector<std::string> &roots);

  // Does nothing while a scan is running.
  void scan_now(const std::string &volume_id = "");

  // Includes untracked connected drives and tracked offline drives. Empty ID is
  // internal.
  std::vector<DiskVolumeStatus> volumes() const;
  Collected<std::string> add_root(const std::string &path);
  Collected<void> set_volume_roots(const std::string &id,
                                   const std::vector<std::string> &roots);
  // Forget tracking preferences, retaining history on the internal disk.
  Collected<void> forget_volume(const std::string &id);

  // Compares the latest scan with the one `since_seconds` before it, as in
  // choose_baseline. Reads from disk, so call it off the main thread.
  Collected<DiskReport> report(std::int64_t since_seconds,
                               const DiskReportLimits &limits = {},
                               const std::string &volume_id = "");

private:
  struct Target {
    DiskVolume volume;
    std::vector<std::string>
        roots; // external paths are volume-relative, starting with /
    DiskTrackerStatus status;
    std::optional<std::int64_t> last_attempt;
    bool active = false, available = false;
  };
  void run(const std::stop_token &stop);
  void scan(const std::string &id, const std::stop_token &stop);
  void publish();
  void reconcile();
  void refresh_history_status(const std::string &id);
  std::string history_dir(const std::string &id) const;
  std::vector<std::int64_t> scan_times(const std::string &id) const;
  Collected<DiskSnapshot> load(const std::string &id, std::int64_t time) const;
  Collected<void> save(const std::string &id,
                       const DiskSnapshot &snapshot) const;
  Collected<void> save_targets() const; // caller holds mutex_
  void load_targets();                  // before worker starts

  std::function<void(const DiskTrackerStatus &)> callback_;
  DiskTrackerOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable_any wakeup_;
  std::map<std::string, Target> targets_;
  std::vector<DiskVolume> mounted_;
  std::set<std::string> requested_;
  std::string configuration_error_;
  mutable std::mutex history_mutex_;
  std::jthread worker_; // last: stops before shared state is destroyed
};

} // namespace hm
