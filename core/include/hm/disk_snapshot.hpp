// Platform-neutral disk tracking: what a scan records, and what changed between
// two scans.
#pragma once

#include "hm/collectors.hpp"
#include "hm/disk_walk.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hm {

inline constexpr std::uint64_t mebibyte = std::uint64_t{1024} * 1024;

// A snapshot only records what is large enough to matter, which keeps it to
// megabytes on a disk with millions of files. A change can therefore be
// overstated by up to these amounts, when something crosses the limit between
// two scans.
inline constexpr std::uint64_t min_recorded_directory = 10 * mebibyte;
inline constexpr std::uint64_t min_recorded_file = 50 * mebibyte;

struct DiskEntry {
  std::string path;
  std::uint64_t bytes = 0;
  bool is_file = false;
  bool is_project = false; // a directory holding a .git entry
};

struct DiskSnapshot {
  std::int64_t time = 0; // seconds since the Unix epoch
  std::uint64_t volume_total = 0, volume_free = 0;
  std::uint64_t file_count = 0;
  std::vector<std::string> roots;
  std::vector<std::string> unreadable;
  std::vector<DiskEntry>
      entries; // sorted by path, so parents come before their children
};

// Absolute paths without trailing slashes, with duplicates and folders inside
// another root removed, so that nothing is counted twice.
std::vector<std::string> normalize_disk_roots(std::vector<std::string> roots);

// Builds a snapshot from a walk.
class DiskSnapshotBuilder final : public DiskWalkVisitor {
public:
  using Progress =
      std::function<void(std::uint64_t files, std::uint64_t bytes)>;

  explicit DiskSnapshotBuilder(Progress progress = {});

  void enter_directory(const std::string &path) override;
  void file(const std::string &path, std::uint64_t bytes) override;
  void leave_directory() override;
  void unreadable(const std::string &path) override;

  // Sorts the entries and hands over the snapshot, leaving the builder empty.
  DiskSnapshot finish();

private:
  struct Directory {
    std::string path;
    std::uint64_t bytes = 0;
    bool is_project = false;
  };

  void mark_project(std::string_view path);

  Progress progress_;
  std::vector<Directory> stack_;
  DiskSnapshot snapshot_;
  std::uint64_t bytes_seen_ = 0;
};

std::string serialize_snapshot(const DiskSnapshot &snapshot);
Collected<DiskSnapshot> parse_snapshot(std::string_view text);

struct DiskRow {
  std::string path;
  std::uint64_t before = 0, after = 0;
  std::optional<std::size_t> parent; // index into DiskReport::rows
  bool is_file = false;
  bool is_project = false;
  // False where the earlier scan did not cover this path, so `before` just
  // repeats `after`.
  bool compared = false;

  [[nodiscard]] std::int64_t change() const
  {
    return static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
  }
};

struct DiskReportLimits {
  std::uint64_t min_change =
      50 * mebibyte; // a smaller change alone does not earn a row
  std::uint64_t min_size = 100 * mebibyte; // nor does a smaller size
  std::size_t max_hotspots = 12;
};

struct DiskReport {
  std::optional<std::int64_t>
      before_time; // empty when there is no earlier scan to compare
  std::int64_t after_time = 0;
  std::uint64_t free_before = 0, free_after = 0, volume_total = 0;
  bool volume_compared = false; // both scans read the volume, so the two
                                // figures below mean something
  std::int64_t scanned_change =
      0; // net change across every folder both scans read
  std::vector<DiskRow>
      rows; // sorted by path, so parents come before their children
  // Rows that grew, largest first, none inside another. They stop at a project,
  // or at the folder where the growth is too spread out to pin on anything
  // smaller.
  std::vector<std::size_t> hotspots;
  // Left out of the comparison, because one of the two scans could not read
  // them.
  std::vector<std::string> unreadable;

  // How much the used space on the volume grew. Compare it with
  // `scanned_change`.
  [[nodiscard]] std::int64_t volume_change() const
  {
    if (!volume_compared)
      return 0;
    return static_cast<std::int64_t>(free_before) -
           static_cast<std::int64_t>(free_after);
  }
  // The growth that happened outside the scanned folders. Can be negative. It
  // includes space macOS holds back as purgeable, and files that share their
  // blocks with a clone.
  [[nodiscard]] std::int64_t unexplained_change() const
  {
    return volume_compared ? volume_change() - scanned_change : 0;
  }
};

// `before` may be null, which gives a report of the sizes in `after` alone.
DiskReport compare_snapshots(const DiskSnapshot *before,
                             const DiskSnapshot &after,
                             const DiskReportLimits &limits = {});

// The newest scan at least `since_seconds` older than the latest, or the oldest
// scan when none is that old. Zero means the scan before the latest. Empty with
// fewer than two scans. `times` must be sorted, oldest first.
std::optional<std::int64_t>
choose_baseline(const std::vector<std::int64_t> &times,
                std::int64_t since_seconds);

// Scans to delete: every scan is kept for 3 days, then the newest of each day,
// up to 90 days. The latest scan is always kept. `times` must be sorted, oldest
// first.
std::vector<std::int64_t> expired_scans(const std::vector<std::int64_t> &times,
                                        std::int64_t now);

} // namespace hm
