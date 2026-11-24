// Scheduling and storage in the disk tracker, with a made-up file tree.
#include "check.hpp"
#include "hm/disk_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::uint64_t mb = hm::mebibyte;
constexpr auto signed_mb = static_cast<std::int64_t>(mb);

class TempDir {
public:
  TempDir()
  {
    path_ = fs::temp_directory_path() /
            ("hm_disk_tracker_test_" + std::to_string(getpid()) + "_" +
             std::to_string(counter_++));
    fs::remove_all(path_);
  }
  ~TempDir()
  {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path &path() const
  {
    return path_;
  }

private:
  static inline int counter_ = 0;
  fs::path path_;
};

// The world the tracker sees, shared with its worker thread.
struct World {
  std::atomic<std::int64_t> now = 1000;
  std::atomic<std::uint64_t> size = 200;
  std::atomic<int> walks = 0;

  hm::DiskTrackerOptions options(const fs::path &dir)
  {
    hm::DiskTrackerOptions out;
    out.history_dir = dir.string();
    out.roots = {"/r/"};
    out.scan_interval = 100s;
    out.max_wait = 1s;
    out.now = [this] { return now.load(); };
    out.disk = [this]() -> hm::Collected<hm::Disk> {
      return hm::Disk{1000 * mb, (500 + size.load()) * mb};
    };
    out.walk = [this](const std::vector<std::string> &roots,
                      hm::DiskWalkVisitor &visitor, const std::stop_token &) {
      ++walks;
      require(roots == std::vector<std::string>{"/r"},
              "the walk gets normalized roots");
      visitor.enter_directory("/r");
      visitor.enter_directory("/r/big");
      visitor.file("/r/big/data", size.load() * mb);
      visitor.leave_directory();
      visitor.leave_directory();
    };
    return out;
  }
};

class Statuses {
public:
  void add(const hm::DiskTrackerStatus &status)
  {
    {
      const std::scoped_lock lock(mutex_);
      latest_ = status;
      ++calls_;
    }
    changed_.notify_all();
  }

  // Waits until the status matches, and fails the test if it never does.
  template <typename Predicate>
  hm::DiskTrackerStatus wait_for(Predicate predicate)
  {
    std::unique_lock lock(mutex_);
    const bool matched =
        changed_.wait_for(lock, 10s, [&] { return predicate(latest_); });
    require(matched, "the tracker reached the expected state in time");
    return latest_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  hm::DiskTrackerStatus latest_;
  int calls_ = 0;
};

void test_scan_and_report()
{
  TempDir dir;
  World world;
  Statuses statuses;
  hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                          world.options(dir.path()));

  require(!tracker.report(0).has_value(),
          "there is nothing to report before a scan");
  tracker.scan_now();
  auto status = statuses.wait_for(
      [](const auto &s) { return !s.scanning && s.scan_count == 1; });
  require(status.last_scan == 1000 && status.error.empty(),
          "the first scan is saved");

  const auto first = tracker.report(0);
  require(first.has_value() && !first->before_time, "one scan has no baseline");

  world.now = 1050;
  world.size = 1200;
  tracker.scan_now();
  statuses.wait_for(
      [](const auto &s) { return !s.scanning && s.scan_count == 2; });

  const auto second = tracker.report(0);
  require(second.has_value() && second->before_time == 1000,
          "two scans can be compared");
  require(second->scanned_change == 1000 * signed_mb, "growth is measured");
  require(second->unexplained_change() == 0,
          "and it accounts for the free space");
  require(!second->hotspots.empty() &&
              second->rows[second->hotspots[0]].path == "/r/big/data",
          "and it is pinned on the file");
}

void test_nothing_scans_by_itself_at_first()
{
  TempDir dir;
  World world;
  Statuses statuses;
  hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                          world.options(dir.path()));
  world.now = 1'000'000;
  std::this_thread::sleep_for(1200ms);
  require(world.walks == 0,
          "the privacy prompts wait until a scan is asked for");
}

void test_history_survives_and_resumes_the_schedule()
{
  TempDir dir;
  World world;
  {
    Statuses statuses;
    hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                            world.options(dir.path()));
    tracker.scan_now();
    statuses.wait_for(
        [](const auto &s) { return !s.scanning && s.scan_count == 1; });
  }
  require(world.walks == 1, "one scan so far");

  world.now = 1000 + 100; // due by the time the tracker starts again
  Statuses statuses;
  hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                          world.options(dir.path()));
  const auto status =
      statuses.wait_for([](const auto &s) { return s.scan_count == 2; });
  require(status.last_scan == 1100, "an overdue scan runs after a restart");
}

void test_failed_save_waits_for_the_next_interval()
{
  TempDir dir;
  World world;
  // A file where the history folder belongs makes every save fail, as a full
  // disk would.
  {
    std::ofstream blocker(dir.path().string() + "_blocker");
  }
  const auto blocked = fs::path(dir.path().string() + "_blocker");
  Statuses statuses;
  hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                          world.options(blocked));
  tracker.scan_now();
  const auto status = statuses.wait_for(
      [](const auto &s) { return !s.scanning && !s.error.empty(); });
  require(status.scan_count == 0, "the failure is reported");
  std::this_thread::sleep_for(1200ms);
  require(world.walks == 1,
          "a failed save does not start another scan straight away");

  world.now = 1000 + 100;
  fs::remove(blocked);
  const auto retried = statuses.wait_for(
      [](const auto &s) { return !s.scanning && s.scan_count == 1; });
  require(retried.error.empty(), "the schedule survives a failed first save");
  fs::remove_all(blocked);
}

void test_stopping_mid_scan_saves_nothing()
{
  TempDir dir;
  World world;
  auto options = world.options(dir.path());
  std::atomic<bool> walking = false;
  options.walk = [&](const std::vector<std::string> &,
                     hm::DiskWalkVisitor &visitor,
                     const std::stop_token &stop) {
    visitor.enter_directory("/r");
    walking = true;
    while (!stop.stop_requested())
      std::this_thread::sleep_for(1ms);
  };
  {
    hm::DiskTracker tracker([](const auto &) {}, std::move(options));
    tracker.scan_now();
    while (!walking)
      std::this_thread::sleep_for(1ms);
  }
  require(!fs::exists(dir.path()) || fs::is_empty(dir.path()),
          "a half-finished scan would read as a huge deletion");
}

void test_old_scans_are_pruned()
{
  TempDir dir;
  World world;
  Statuses statuses;
  hm::DiskTracker tracker([&](const auto &status) { statuses.add(status); },
                          world.options(dir.path()));
  tracker.scan_now();
  statuses.wait_for(
      [](const auto &s) { return !s.scanning && s.scan_count == 1; });
  world.now = 1000 + 100 * 86400;
  tracker.scan_now();
  // The new scan replaces the old one, so the count stays at one with a later
  // time.
  const auto status = statuses.wait_for([&](const auto &s) {
    return !s.scanning && s.last_scan == world.now.load();
  });
  require(status.scan_count == 1, "a scan past the retention limit is deleted");
}

// Synthetic drives exercise removal and remounts without touching real external
// data.
struct DriveWorld {
  std::mutex mutex;
  std::vector<hm::DiskVolume> mounted{
      {"drive-a", "Backup", "/Volumes/Backup", "mount-1", 2000 * mb, 1000 * mb},
      {"drive-b", "Backup", "/Volumes/Backup 1", "mount-2", 4000 * mb,
       3000 * mb}};
  std::atomic<std::int64_t> now = 1000;
  std::atomic<std::uint64_t> size = 200;
  std::atomic<int> walks = 0;
  std::atomic<bool> remove_during_walk = false;
  std::atomic<bool> unreadable_root = false;
  std::atomic<int> concurrent = 0, maximum = 0;

  hm::DiskTrackerOptions options(const fs::path &dir)
  {
    hm::DiskTrackerOptions out;
    out.history_dir = dir.string();
    out.roots = {};
    out.max_wait = 1s;
    out.scan_interval = 100s;
    out.now = [this] { return now.load(); };
    out.volumes = [this]() -> hm::Collected<std::vector<hm::DiskVolume>> {
      const std::scoped_lock lock(mutex);
      return mounted;
    };
    out.volume_for_path =
        [this](const std::string &path) -> hm::Collected<hm::DiskVolume> {
      const std::scoped_lock lock(mutex);
      for (const auto &volume : mounted)
        if (path == volume.mount_path ||
            path.starts_with(volume.mount_path + "/"))
          return volume;
      return std::unexpected("Drive unavailable");
    };
    out.walk = [this](const std::vector<std::string> &roots,
                      hm::DiskWalkVisitor &visitor, const std::stop_token &) {
      const auto count = ++concurrent;
      maximum = std::max(maximum.load(), count);
      ++walks;
      for (const auto &root : roots) {
        if (unreadable_root) {
          visitor.unreadable(root);
          continue;
        }
        visitor.enter_directory(root);
        visitor.file(root + "/data", size.load() * mb);
        visitor.leave_directory();
      }
      if (remove_during_walk.exchange(false)) {
        const std::scoped_lock lock(mutex);
        mounted.clear();
      }
      --concurrent;
    };
    return out;
  }
};

hm::DiskVolumeStatus drive_status(hm::DiskTracker &tracker,
                                  const std::string &id)
{
  for (const auto &volume : tracker.volumes())
    if (volume.volume.id == id)
      return volume;
  require(false, "expected tracked drive");
  return {};
}

template <typename Predicate>
void wait_drive(hm::DiskTracker &tracker, const std::string &id,
                Predicate predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate(drive_status(tracker, id)))
      return;
    std::this_thread::sleep_for(10ms);
  }
  require(false, "drive reached expected state in time");
}

void test_external_histories_and_remount()
{
  TempDir dir;
  DriveWorld world;
  {
    hm::DiskTracker tracker([](const auto &) {}, world.options(dir.path()));
    require(tracker.add_root("/Volumes/Backup/projects") == "drive-a",
            "enroll by volume identity");
    require(tracker.add_root("/Volumes/Backup/projects/nested") == "drive-a",
            "nested roots accepted");
    require(drive_status(tracker, "drive-a").roots.size() == 1,
            "nested roots deduplicated");
    require(tracker.add_root("/Volumes/Backup 1") == "drive-b",
            "same names have different identities");
    require(world.walks == 0, "adding drives does not start scans");
    tracker.scan_now("drive-a");
    tracker.scan_now("drive-b");
    wait_drive(tracker, "drive-a", [](const auto &v) {
      return v.status.scan_count == 1 && !v.status.scanning;
    });
    wait_drive(tracker, "drive-b", [](const auto &v) {
      return v.status.scan_count == 1 && !v.status.scanning;
    });
    require(world.maximum == 1, "drive walks never overlap");
    const auto a = tracker.report(0, {}, "drive-a");
    const auto b = tracker.report(0, {}, "drive-b");
    require(a && b && a->volume_total == 2000 * mb &&
                b->volume_total == 4000 * mb,
            "capacity belongs to each drive");
    require(!tracker.report(0),
            "external history does not replace internal history");
    {
      const std::scoped_lock lock(world.mutex);
      world.mounted[0].mount_path = "/Volumes/Renamed";
      world.mounted[0].name = "Renamed";
      world.mounted[0].session = "mount-3";
    }
    world.now = 1050;
    world.size = 400;
    tracker.scan_now("drive-a");
    wait_drive(tracker, "drive-a", [](const auto &v) {
      return v.status.scan_count == 2 && !v.status.scanning;
    });
    const auto changed = tracker.report(0, {}, "drive-a");
    require(changed && changed->scanned_change == 200 * signed_mb,
            "relative paths preserve growth on rename");
    require(!changed->rows.empty() &&
                changed->rows.front().path.starts_with("/Volumes/Renamed/"),
            "Finder paths use the current mount point");
    world.remove_during_walk = true;
    world.now = 1060;
    tracker.scan_now("drive-a");
    wait_drive(tracker, "drive-a", [](const auto &v) {
      return !v.status.scanning && !v.status.error.empty();
    });
    require(drive_status(tracker, "drive-a").status.scan_count == 2,
            "partial scan is discarded after removal");
    require(tracker.report(0, {}, "drive-a")->scanned_change == 200 * signed_mb,
            "disconnect retains last valid comparison");
  }
  // Restore from local metadata with no attached drives.
  {
    hm::DiskTracker tracker([](const auto &) {}, world.options(dir.path()));
    wait_drive(tracker, "drive-a",
               [](const auto &v) { return v.status.scan_count == 2; });
    require(!drive_status(tracker, "drive-a").available,
            "offline drive remains tracked across restart");
    require(tracker.report(0, {}, "drive-a").has_value(),
            "offline history remains readable");
    {
      const std::scoped_lock lock(world.mutex);
      world.mounted = {{"new-filesystem", "Renamed", "/Volumes/Renamed",
                        "replacement", 5000 * mb, 4000 * mb}};
    }
    tracker.scan_now("drive-a");
    wait_drive(tracker, "drive-a",
               [](const auto &v) { return !v.status.error.empty(); });
    require(drive_status(tracker, "drive-a").status.scan_count == 2,
            "replacement is not the old filesystem");
    require(tracker.add_root("/Volumes/Renamed") == "new-filesystem",
            "reformatted drive is a new target");
    require(!tracker.report(0, {}, "new-filesystem"),
            "replacement has a fresh baseline");
    require(tracker.forget_volume("drive-a").has_value(),
            "forget tracking without deleting history");
  }
}

void test_external_unreadable_and_duplicate_identity()
{
  TempDir dir;
  DriveWorld world;
  hm::DiskTracker tracker([](const auto &) {}, world.options(dir.path()));
  require(tracker.add_root("/Volumes/Backup").has_value(), "drive enrolls");
  world.unreadable_root = true;
  tracker.scan_now("drive-a");
  wait_drive(tracker, "drive-a",
             [](const auto &v) { return !v.status.error.empty(); });
  require(!tracker.report(0, {}, "drive-a"),
          "denied root does not save empty history");
  world.unreadable_root = false;
  {
    const std::scoped_lock lock(world.mutex);
    auto clone = world.mounted[0];
    clone.mount_path = "/Volumes/Clone";
    world.mounted.push_back(clone);
  }
  tracker.scan_now("drive-a");
  wait_drive(tracker, "drive-a", [](const auto &v) { return !v.available; });
  require(!tracker.report(0, {}, "drive-a"),
          "ambiguous UUID cannot be scanned");
}

void test_external_schedule_and_reconnect()
{
  TempDir dir;
  DriveWorld world;
  hm::DiskTracker tracker([](const auto &) {}, world.options(dir.path()));
  require(tracker.add_root("/Volumes/Backup").has_value(),
          "first drive enrolls");
  require(tracker.add_root("/Volumes/Backup 1").has_value(),
          "second drive enrolls");
  tracker.scan_now("drive-a");
  wait_drive(tracker, "drive-a", [](const auto &v) {
    return !v.status.scanning && v.status.scan_count == 1;
  });
  std::vector<hm::DiskVolume> previous;
  {
    const std::scoped_lock lock(world.mutex);
    previous = world.mounted;
    world.mounted.clear();
  }
  world.now = 1100;
  wait_drive(tracker, "drive-a", [](const auto &v) { return !v.available; });
  require(world.walks == 1, "offline scheduled work is skipped");
  {
    const std::scoped_lock lock(world.mutex);
    world.mounted = std::move(previous);
  }
  wait_drive(tracker, "drive-a", [](const auto &v) {
    return !v.status.scanning && v.status.scan_count == 2;
  });
  require(world.walks == 2 &&
              drive_status(tracker, "drive-b").status.scan_count == 0,
          "overdue drive resumes and unstarted drive does not scan");
}

void test_scan_due()
{
  require(!hm::disk_scan_due(std::nullopt, 100s, 1000),
          "never before the first scan");
  require(!hm::disk_scan_due(950, 100s, 1000), "not before the interval");
  require(hm::disk_scan_due(900, 100s, 1000), "at the interval");
}

} // namespace

int main()
{
  test_external_schedule_and_reconnect();
  test_external_histories_and_remount();
  test_external_unreadable_and_duplicate_identity();
  test_scan_due();
  test_scan_and_report();
  test_nothing_scans_by_itself_at_first();
  test_history_survives_and_resumes_the_schedule();
  test_failed_save_waits_for_the_next_interval();
  test_stopping_mid_scan_saves_nothing();
  test_old_scans_are_pruned();
}
