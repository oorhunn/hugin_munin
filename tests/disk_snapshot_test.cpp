// The disk tracker's bookkeeping, with no backend.
#include "check.hpp"
#include "hm/disk_snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t mb = hm::mebibyte;
constexpr std::uint64_t gb = 1024 * mb;
constexpr auto signed_gb = static_cast<std::int64_t>(gb);

hm::DiskEntry dir(std::string path, std::uint64_t bytes, bool project = false)
{
  return {std::move(path), bytes, false, project};
}

hm::DiskEntry file(std::string path, std::uint64_t bytes)
{
  return {std::move(path), bytes, true, false};
}

hm::DiskSnapshot snapshot(std::int64_t time, std::vector<hm::DiskEntry> entries,
                          std::vector<std::string> unreadable = {},
                          std::vector<std::string> roots = {"/home"})
{
  hm::DiskSnapshot out;
  out.time = time;
  out.volume_total = 1000 * gb;
  out.volume_free = 100 * gb;
  out.roots = std::move(roots);
  out.unreadable = std::move(unreadable);
  out.entries = std::move(entries);
  std::ranges::sort(out.entries, {}, &hm::DiskEntry::path);
  return out;
}

const hm::DiskRow *find(const hm::DiskReport &report, std::string_view path)
{
  const auto row = std::ranges::find(report.rows, path, &hm::DiskRow::path);
  return row == report.rows.end() ? nullptr : &*row;
}

std::vector<std::string> hotspots(const hm::DiskReport &report)
{
  std::vector<std::string> out;
  out.reserve(report.hotspots.size());
  for (const auto index : report.hotspots)
    out.push_back(report.rows[index].path);
  return out;
}

void test_normalizes_roots()
{
  using Roots = std::vector<std::string>;
  require(hm::normalize_disk_roots({"/a/", "/a/b", "/a-b", "relative", "",
                                    "/a"}) == Roots{"/a", "/a-b"},
          "roots drop duplicates, nested folders, and relative paths");
  require(hm::normalize_disk_roots({"/", "/a"}) == Roots{"/"},
          "everything is under /");
}

void test_builds_recursive_sizes()
{
  hm::DiskSnapshotBuilder builder;
  builder.enter_directory("/r");
  builder.file("/r/small", 1 * mb);
  builder.enter_directory("/r/project");
  builder.enter_directory("/r/project/.git");
  builder.file("/r/project/.git/pack", 20 * mb);
  builder.leave_directory();
  builder.file("/r/project/video", 60 * mb);
  builder.leave_directory();
  builder.enter_directory("/r/tiny");
  builder.file("/r/tiny/a", 2 * mb);
  builder.leave_directory();
  builder.unreadable("/r/secret");
  builder.leave_directory();
  const auto result = builder.finish();

  require(result.file_count == 4, "every file is counted");
  require(result.unreadable == std::vector<std::string>{"/r/secret"},
          "unreadable is recorded");

  const auto entry = [&](std::string_view path) -> const hm::DiskEntry * {
    const auto found =
        std::ranges::find(result.entries, path, &hm::DiskEntry::path);
    return found == result.entries.end() ? nullptr : &*found;
  };
  require(entry("/r") && entry("/r")->bytes == 83 * mb,
          "a folder adds up what is inside it");
  require(entry("/r/project") && entry("/r/project")->is_project,
          "a .git folder marks a project");
  require(entry("/r/project/.git") && !entry("/r/project/.git")->is_project,
          "the .git folder is not a project itself");
  require(entry("/r/project/video") && entry("/r/project/video")->is_file,
          "large file recorded");
  require(!entry("/r/tiny") && !entry("/r/small"),
          "small entries are not recorded");
  require(std::ranges::is_sorted(result.entries, {}, &hm::DiskEntry::path),
          "entries are sorted");
}

void test_linked_worktree_is_a_project()
{
  hm::DiskSnapshotBuilder builder;
  builder.enter_directory("/r");
  builder.enter_directory("/r/tree");
  builder.file("/r/tree/.git", 4096);
  builder.file("/r/tree/data", 20 * mb);
  builder.leave_directory();
  builder.leave_directory();
  const auto result = builder.finish();
  const auto tree =
      std::ranges::find(result.entries, "/r/tree", &hm::DiskEntry::path);
  require(tree != result.entries.end() && tree->is_project,
          "a .git file also marks a project");
}

void test_round_trips_through_text()
{
  auto original = snapshot(
      1234, {dir("/home", 3 * gb), dir("/home/a b", 2 * gb, true),
             dir("/home/line\nbreak\\", 1 * gb), file("/home/f", 60 * mb)});
  original.file_count = 99;
  original.unreadable = {"/home/no\naccess"};

  const auto parsed = hm::parse_snapshot(hm::serialize_snapshot(original));
  require(parsed.has_value(), "a snapshot parses");
  require(parsed->time == 1234 && parsed->file_count == 99, "numbers survive");
  require(parsed->volume_total == original.volume_total,
          "volume total survives");
  require(parsed->volume_free == original.volume_free, "volume free survives");
  require(parsed->roots == original.roots, "roots survive");
  require(parsed->unreadable == original.unreadable, "unusual paths survive");
  require(parsed->entries.size() == original.entries.size(),
          "every entry survives");
  for (std::size_t i = 0; i < original.entries.size(); ++i) {
    require(parsed->entries[i].path == original.entries[i].path, "entry path");
    require(parsed->entries[i].bytes == original.entries[i].bytes,
            "entry size");
    require(parsed->entries[i].is_file == original.entries[i].is_file,
            "entry kind");
    require(parsed->entries[i].is_project == original.entries[i].is_project,
            "entry project");
  }
}

void test_rejects_damaged_text()
{
  require(!hm::parse_snapshot("").has_value(), "empty text");
  require(!hm::parse_snapshot("hm-disk-snapshot 2\n").has_value(),
          "unknown version");
  require(!hm::parse_snapshot("hm-disk-snapshot 1\nd nope /a\n").has_value(),
          "bad number");
  require(!hm::parse_snapshot("hm-disk-snapshot 1\nwhat 1\n").has_value(),
          "unknown record");
  require(!hm::parse_snapshot("hm-disk-snapshot 1\nd 1 /a").has_value(),
          "truncated record");
  require(!hm::parse_snapshot("hm-disk-snapshot 1\nroot /a\\x\n").has_value(),
          "bad escape");
}

void test_report_without_a_baseline()
{
  const auto after =
      snapshot(100, {dir("/home", 3 * gb), dir("/home/a", 3 * gb)});
  const auto report = hm::compare_snapshots(nullptr, after);
  require(!report.before_time.has_value(), "no baseline");
  require(report.rows.size() == 2, "sizes are still listed");
  require(report.rows[0].change() == 0 && !report.rows[0].compared,
          "no change is claimed");
  require(report.rows[1].parent == 0, "rows link to their parent");
  require(report.hotspots.empty() && report.scanned_change == 0,
          "nothing can be blamed");
  require(!report.volume_compared && report.volume_change() == 0,
          "free space has no history");
  require(report.unexplained_change() == 0, "so nothing is unexplained");
}

void test_failed_volume_read_claims_no_change()
{
  auto before = snapshot(100, {dir("/home", 1 * gb)});
  before.volume_total = before.volume_free = 0;
  const auto after = snapshot(200, {dir("/home", 2 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(!report.volume_compared, "an unread volume cannot be compared");
  require(report.volume_change() == 0 && report.unexplained_change() == 0,
          "or explained");
  require(report.scanned_change == 1 * signed_gb, "folders still compare");
}

void test_finds_what_grew()
{
  const auto before =
      snapshot(100, {dir("/home", 10 * gb), dir("/home/projects", 8 * gb),
                     dir("/home/projects/app", 2 * gb, true),
                     dir("/home/projects/game", 6 * gb, true)});
  auto after =
      snapshot(200, {dir("/home", 22 * gb), dir("/home/projects", 20 * gb),
                     dir("/home/projects/app", 11 * gb, true),
                     dir("/home/projects/app/target", 9 * gb),
                     dir("/home/projects/game", 9 * gb, true)});
  after.volume_free = 85 * gb;
  const auto report = hm::compare_snapshots(&before, after);

  require(report.before_time == 100, "the baseline is reported");
  require(report.scanned_change == 12 * signed_gb,
          "the total counts /home once rather than adding up nested rows");
  require(report.volume_change() == 15 * signed_gb, "free space fell 15 GB");
  require(report.unexplained_change() == 3 * signed_gb,
          "what scanning does not explain is reported");
  require(hotspots(report) == std::vector<std::string>{"/home/projects/app",
                                                       "/home/projects/game"},
          "growth stops at the projects, largest first, with nothing nested");
}

void test_spread_out_growth_stops_at_the_folder()
{
  const auto before =
      snapshot(100, {dir("/home", 1 * gb), dir("/home/cache", 1 * gb)});
  // /home/cache grew 10 GB, but no recorded child accounts for most of it.
  const auto after =
      snapshot(200, {dir("/home", 11 * gb), dir("/home/cache", 11 * gb),
                     dir("/home/cache/one", 60 * mb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(hotspots(report) == std::vector<std::string>{"/home/cache"},
          "the folder is blamed rather than a child that barely matters");
}

void test_file_can_be_a_hotspot()
{
  const auto before =
      snapshot(100, {dir("/home", 1 * gb), dir("/home/Downloads", 1 * gb)});
  const auto after =
      snapshot(200, {dir("/home", 21 * gb), dir("/home/Downloads", 21 * gb),
                     file("/home/Downloads/disk.iso", 20 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(hotspots(report) ==
              std::vector<std::string>{"/home/Downloads/disk.iso"},
          "one large file is named directly");
}

void test_a_git_home_folder_still_descends()
{
  const auto before =
      snapshot(100, {dir("/home", 1 * gb, true), dir("/home/a", 1 * gb)});
  const auto after =
      snapshot(200, {dir("/home", 9 * gb, true), dir("/home/a", 9 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(hotspots(report) == std::vector<std::string>{"/home/a"},
          "a root is never the answer just because it holds a .git folder");
}

void test_shrinkage_is_not_a_hotspot()
{
  const auto before =
      snapshot(100, {dir("/home", 9 * gb), dir("/home/a", 9 * gb)});
  const auto after = snapshot(200, {dir("/home", 1 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(report.hotspots.empty(), "deleting something consumed no space");
  const auto *row = find(report, "/home/a");
  require(row && row->after == 0 && row->change() == -9 * signed_gb,
          "a deleted folder shows as a loss");
}

void test_unreadable_folder_is_not_a_deletion()
{
  // /home/mail held 5 GB, and the second scan lost access to it.
  const auto before = snapshot(
      100, {dir("/home", 6 * gb), dir("/home/mail", 5 * gb),
            dir("/home/mail/inbox", 5 * gb), dir("/home/work", 1 * gb)});
  const auto after = snapshot(
      200, {dir("/home", 2 * gb), dir("/home/work", 2 * gb)}, {"/home/mail"});
  const auto report = hm::compare_snapshots(&before, after);

  require(!find(report, "/home/mail") && !find(report, "/home/mail/inbox"),
          "the folder and its contents leave the comparison");
  require(report.unreadable == std::vector<std::string>{"/home/mail"},
          "and it is reported");
  require(
      report.scanned_change == 1 * signed_gb,
      "its parents do not show a 5 GB loss, and the real growth still shows");
  require(hotspots(report) == std::vector<std::string>{"/home/work"},
          "the growth is found");
}

void test_unreadable_in_the_earlier_scan_is_not_growth()
{
  const auto before = snapshot(
      100, {dir("/home", 1 * gb), dir("/home/work", 1 * gb)}, {"/home/mail"});
  const auto after =
      snapshot(200, {dir("/home", 6 * gb), dir("/home/mail", 5 * gb),
                     dir("/home/work", 1 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(report.scanned_change == 0, "gaining access is not growth");
  require(report.hotspots.empty(), "and blames nothing");
}

void test_nested_unreadable_only_counts_the_top()
{
  const auto after = snapshot(200, {dir("/home", 1 * gb)},
                              {"/home/a", "/home/a/b", "/home/a-b"});
  const auto report = hm::compare_snapshots(nullptr, after);
  require(report.unreadable == std::vector<std::string>{"/home/a", "/home/a-b"},
          "a path inside another unreadable path is dropped, but not a similar "
          "name");
}

void test_a_new_root_is_not_growth()
{
  const auto before = snapshot(100, {dir("/home", 1 * gb)});
  const auto after = snapshot(200, {dir("/home", 2 * gb), dir("/opt", 30 * gb)},
                              {}, {"/home", "/opt"});
  const auto report = hm::compare_snapshots(&before, after);
  require(report.scanned_change == 1 * signed_gb, "/opt adds no change");
  const auto *opt = find(report, "/opt");
  require(opt && !opt->compared && opt->change() == 0,
          "/opt is listed with its size");
}

void test_widened_root_uses_the_folder_both_scans_covered()
{
  const auto before =
      snapshot(100, {dir("/home/code", 1 * gb)}, {}, {"/home/code"});
  const auto after =
      snapshot(200, {dir("/home", 40 * gb), dir("/home/code", 3 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(report.scanned_change == 2 * signed_gb, "only the overlap counts");
  require(hotspots(report) == std::vector<std::string>{"/home/code"},
          "growth is found in it");
}

void test_removed_root_is_dropped()
{
  const auto before = snapshot(
      100, {dir("/home", 1 * gb), dir("/opt", 30 * gb)}, {}, {"/home", "/opt"});
  const auto after = snapshot(200, {dir("/home", 1 * gb)});
  const auto report = hm::compare_snapshots(&before, after);
  require(!find(report, "/opt"),
          "a folder no longer scanned is not a deletion");
  require(report.scanned_change == 0, "and it does not count as a loss");
}

void test_limits_keep_the_tree_connected()
{
  hm::DiskReportLimits limits;
  limits.min_change = 50 * mb;
  limits.min_size = 100 * mb;
  const auto before =
      snapshot(100, {dir("/home", 40 * mb), dir("/home/a", 20 * mb),
                     dir("/home/a/b", 20 * mb), dir("/home/quiet", 20 * mb)});
  const auto after =
      snapshot(200, {dir("/home", 95 * mb), dir("/home/a", 75 * mb),
                     dir("/home/a/b", 75 * mb), dir("/home/quiet", 20 * mb)});
  const auto report = hm::compare_snapshots(&before, after, limits);
  require(find(report, "/home/a/b") && find(report, "/home/a") &&
              find(report, "/home"),
          "a row keeps its parents");
  require(!find(report, "/home/quiet"), "a small quiet folder has no row");
  const auto *leaf = find(report, "/home/a/b");
  require(leaf->parent && report.rows[*leaf->parent].path == "/home/a",
          "parents are remapped");
}

void test_hotspot_limit()
{
  hm::DiskReportLimits limits;
  limits.max_hotspots = 2;
  const auto before = snapshot(100, {dir("/home", 1 * mb)});
  const auto after =
      snapshot(200, {dir("/home", 9 * gb), dir("/home/a", 1 * gb),
                     dir("/home/b", 3 * gb), dir("/home/c", 5 * gb)});
  const auto report = hm::compare_snapshots(&before, after, limits);
  require(hotspots(report) == std::vector<std::string>{"/home/c", "/home/b"},
          "largest first");
}

void test_chooses_a_baseline()
{
  using Times = std::vector<std::int64_t>;
  require(!hm::choose_baseline({}, 0), "no scans");
  require(!hm::choose_baseline({100}, 50), "one scan");
  require(hm::choose_baseline(Times{10, 20, 30}, 0) == 20,
          "zero means the previous scan");
  require(hm::choose_baseline(Times{10, 20, 30}, 15) == 10,
          "newest at least that old");
  require(hm::choose_baseline(Times{10, 20, 30}, 10) == 20,
          "an exact match counts");
  require(hm::choose_baseline(Times{10, 20, 30}, 500) == 10,
          "falls back to the oldest");
  require(hm::choose_baseline(Times{10, 20, 30}, 5) == 20,
          "never the latest itself");
}

void test_expires_old_scans()
{
  constexpr std::int64_t day = 86400;
  const std::int64_t now = 1000 * day;
  using Times = std::vector<std::int64_t>;

  const Times recent{now - 2 * day, now - day, now - day + 1, now};
  require(hm::expired_scans(recent, now).empty(), "recent scans all stay");

  const auto old_day = now - 10 * day;
  const Times same_day{old_day, old_day + 100, old_day + 200, now};
  require(hm::expired_scans(same_day, now) == Times{old_day + 100, old_day},
          "the newest scan of an old day stays");

  require(hm::expired_scans(Times{now - 100 * day, now}, now) ==
              Times{now - 100 * day},
          "past 90 days a scan goes");
  require(hm::expired_scans(Times{now - 100 * day}, now).empty(),
          "the latest scan always stays");
}

} // namespace

int main()
{
  test_normalizes_roots();
  test_builds_recursive_sizes();
  test_linked_worktree_is_a_project();
  test_round_trips_through_text();
  test_rejects_damaged_text();
  test_report_without_a_baseline();
  test_failed_volume_read_claims_no_change();
  test_finds_what_grew();
  test_spread_out_growth_stops_at_the_folder();
  test_file_can_be_a_hotspot();
  test_a_git_home_folder_still_descends();
  test_shrinkage_is_not_a_hotspot();
  test_unreadable_folder_is_not_a_deletion();
  test_unreadable_in_the_earlier_scan_is_not_growth();
  test_nested_unreadable_only_counts_the_top();
  test_a_new_root_is_not_growth();
  test_widened_root_uses_the_folder_both_scans_covered();
  test_removed_root_is_dropped();
  test_limits_keep_the_tree_connected();
  test_hotspot_limit();
  test_chooses_a_baseline();
  test_expires_old_scans();
}
