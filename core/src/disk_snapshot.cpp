#include "hm/disk_snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hm {
namespace {

constexpr std::string_view snapshot_header = "hm-disk-snapshot 1";
constexpr std::int64_t seconds_per_day = 86400;
constexpr std::int64_t keep_everything_days = 3;
constexpr std::int64_t keep_daily_days = 90;

bool is_under(std::string_view path, std::string_view folder)
{
  if (folder == "/")
    return path.starts_with('/');
  return path.starts_with(folder) &&
         (path.size() == folder.size() || path[folder.size()] == '/');
}

bool is_under_any(std::string_view path,
                  const std::vector<std::string> &folders)
{
  return std::ranges::any_of(folders, [&](const std::string &folder) {
    return is_under(path, folder);
  });
}

// Empty for "/" and for a path with no slash.
std::string_view parent_of(std::string_view path)
{
  if (path == "/")
    return {};
  const auto slash = path.rfind('/');
  if (slash == std::string_view::npos)
    return {};
  return path.substr(0, std::max<std::size_t>(slash, 1));
}

std::string_view name_of(std::string_view path)
{
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Paths may hold any byte except NUL, and a line break would split a record.
std::string escape_path(std::string_view path)
{
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else
      out += c;
  }
  return out;
}

std::optional<std::string> unescape_path(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\') {
      out += text[i];
      continue;
    }
    if (++i == text.size())
      return std::nullopt;
    if (text[i] == '\\')
      out += '\\';
    else if (text[i] == 'n')
      out += '\n';
    else if (text[i] == 'r')
      out += '\r';
    else
      return std::nullopt;
  }
  return out;
}

// Reads a number and the single space after it, if any, from the front of
// `text`.
template <typename T> std::optional<T> take_number(std::string_view &text)
{
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{})
    return std::nullopt;
  text.remove_prefix(static_cast<std::size_t>(end - text.data()));
  if (text.starts_with(' '))
    text.remove_prefix(1);
  return value;
}

// One row of the comparison, before limits are applied. Paths point into the
// snapshots.
struct Merged {
  std::uint64_t before = 0, after = 0;
  bool is_file = false, is_project = false;
};

using MergedRows = std::map<std::string_view, Merged>;

MergedRows merge_entries(const DiskSnapshot *before, const DiskSnapshot &after)
{
  MergedRows rows;
  if (before) {
    for (const auto &entry : before->entries) {
      auto &row = rows[entry.path];
      row.before = entry.bytes;
      row.is_file = entry.is_file;
      row.is_project = entry.is_project;
    }
  }
  for (const auto &entry : after.entries) {
    auto &row = rows[entry.path];
    row.after = entry.bytes;
    row.is_file = entry.is_file;
    row.is_project = row.is_project || entry.is_project;
  }
  return rows;
}

// The topmost paths either scan could not read, which must drop out of the
// comparison. A folder that could not be read would otherwise look like it had
// been deleted.
std::vector<std::string_view> excluded_paths(const DiskSnapshot *before,
                                             const DiskSnapshot &after)
{
  std::unordered_set<std::string_view> all(after.unreadable.begin(),
                                           after.unreadable.end());
  if (before)
    all.insert(before->unreadable.begin(), before->unreadable.end());

  std::vector<std::string_view> topmost;
  for (const auto path : all) {
    bool nested = false;
    for (auto folder = parent_of(path); !folder.empty() && !nested;
         folder = parent_of(folder))
      nested = all.contains(folder);
    if (!nested)
      topmost.push_back(path);
  }
  std::ranges::sort(topmost);
  return topmost;
}

// Takes `path`'s size out of its parents and then drops it and everything
// inside it.
void exclude(MergedRows &rows, std::string_view path)
{
  const auto found = rows.find(path);
  if (found != rows.end()) {
    const auto before = found->second.before;
    const auto after = found->second.after;
    for (auto folder = parent_of(path); !folder.empty();
         folder = parent_of(folder)) {
      const auto parent = rows.find(folder);
      if (parent == rows.end())
        continue;
      parent->second.before -= std::min(before, parent->second.before);
      parent->second.after -= std::min(after, parent->second.after);
    }
  }
  for (auto it = rows.lower_bound(path);
       it != rows.end() && it->first.starts_with(path);) {
    it = is_under(it->first, path) ? rows.erase(it) : std::next(it);
  }
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of a path
void find_hotspots(const std::vector<DiskRow> &rows,
                   const std::vector<std::vector<std::size_t>> &children,
                   std::size_t index, bool is_top,
                   const DiskReportLimits &limits,
                   std::vector<std::size_t> &out)
{
  const auto &row = rows[index];
  const auto min_change = static_cast<std::int64_t>(limits.min_change);
  if (row.change() < min_change)
    return;

  std::int64_t explained = 0;
  for (const auto child : children[index])
    if (rows[child].change() >= min_change)
      explained += rows[child].change();

  // Below a project the detail stays in the tree: the project is the answer the
  // user wants.
  const bool stop_at_project = row.is_project && !is_top;
  if (stop_at_project || explained * 2 < row.change()) {
    out.push_back(index);
    return;
  }
  for (const auto child : children[index])
    find_hotspots(rows, children, child, false, limits, out);
}

} // namespace

std::vector<std::string> normalize_disk_roots(std::vector<std::string> roots)
{
  std::vector<std::string> cleaned;
  for (auto &root : roots) {
    while (root.size() > 1 && root.ends_with('/'))
      root.pop_back();
    if (root.starts_with('/'))
      cleaned.push_back(std::move(root));
  }
  std::ranges::sort(cleaned);

  std::vector<std::string> kept;
  for (auto &root : cleaned)
    if (!is_under_any(root, kept))
      kept.push_back(std::move(root));
  return kept;
}

DiskSnapshotBuilder::DiskSnapshotBuilder(Progress progress)
    : progress_(std::move(progress))
{
}

void DiskSnapshotBuilder::mark_project(std::string_view path)
{
  if (name_of(path) == ".git" && !stack_.empty())
    stack_.back().is_project = true;
}

void DiskSnapshotBuilder::enter_directory(const std::string &path)
{
  mark_project(path);
  stack_.push_back({path});
}

void DiskSnapshotBuilder::file(const std::string &path, std::uint64_t bytes)
{
  mark_project(
      path); // a linked Git worktree has a .git file rather than a folder
  if (!stack_.empty())
    stack_.back().bytes += bytes;
  if (bytes >= min_recorded_file)
    snapshot_.entries.push_back({path, bytes, true, false});
  bytes_seen_ += bytes;
  if (++snapshot_.file_count % 4096 == 0 && progress_)
    progress_(snapshot_.file_count, bytes_seen_);
}

void DiskSnapshotBuilder::leave_directory()
{
  if (stack_.empty())
    return;
  auto directory = std::move(stack_.back());
  stack_.pop_back();
  if (stack_.empty() || directory.bytes >= min_recorded_directory)
    snapshot_.entries.push_back({std::move(directory.path), directory.bytes,
                                 false, directory.is_project});
  if (!stack_.empty())
    stack_.back().bytes += directory.bytes;
}

void DiskSnapshotBuilder::unreadable(const std::string &path)
{
  snapshot_.unreadable.push_back(path);
}

DiskSnapshot DiskSnapshotBuilder::finish()
{
  std::ranges::sort(snapshot_.entries, {}, &DiskEntry::path);
  std::ranges::sort(snapshot_.unreadable);
  auto snapshot = std::move(snapshot_);
  snapshot_ = {};
  stack_.clear();
  bytes_seen_ = 0;
  return snapshot;
}

std::string serialize_snapshot(const DiskSnapshot &snapshot)
{
  std::string out(snapshot_header);
  out += '\n';
  out += "time " + std::to_string(snapshot.time) + '\n';
  out += "volume " + std::to_string(snapshot.volume_total) + ' ' +
         std::to_string(snapshot.volume_free) + '\n';
  out += "files " + std::to_string(snapshot.file_count) + '\n';
  for (const auto &root : snapshot.roots)
    out += "root " + escape_path(root) + '\n';
  for (const auto &path : snapshot.unreadable)
    out += "unreadable " + escape_path(path) + '\n';
  for (const auto &entry : snapshot.entries) {
    const char *kind = "d ";
    if (entry.is_file)
      kind = "f ";
    else if (entry.is_project)
      kind = "p ";
    out += kind + std::to_string(entry.bytes) + ' ' + escape_path(entry.path) +
           '\n';
  }
  return out;
}

Collected<DiskSnapshot> parse_snapshot(std::string_view text)
{
  const auto take_line = [&text]() -> std::optional<std::string_view> {
    const auto end = text.find('\n');
    if (end == std::string_view::npos)
      return std::nullopt;
    const auto line = text.substr(0, end);
    text.remove_prefix(end + 1);
    return line;
  };
  const auto invalid = [](std::string_view line) {
    return std::unexpected("Invalid disk snapshot line: " + std::string(line));
  };

  if (take_line() != snapshot_header)
    return std::unexpected("Not a disk snapshot of a known version");

  DiskSnapshot snapshot;
  while (const auto line = take_line()) {
    const auto space = line->find(' ');
    if (space == std::string_view::npos)
      return invalid(*line);
    const auto tag = line->substr(0, space);
    auto value = line->substr(space + 1);

    if (tag == "time") {
      const auto number = take_number<std::int64_t>(value);
      if (!number)
        return invalid(*line);
      snapshot.time = *number;
    } else if (tag == "volume") {
      const auto total = take_number<std::uint64_t>(value);
      const auto free = take_number<std::uint64_t>(value);
      if (!total || !free)
        return invalid(*line);
      snapshot.volume_total = *total;
      snapshot.volume_free = *free;
    } else if (tag == "files") {
      const auto number = take_number<std::uint64_t>(value);
      if (!number)
        return invalid(*line);
      snapshot.file_count = *number;
    } else if (tag == "root" || tag == "unreadable") {
      auto path = unescape_path(value);
      if (!path)
        return invalid(*line);
      (tag == "root" ? snapshot.roots : snapshot.unreadable)
          .push_back(std::move(*path));
    } else if (tag == "d" || tag == "p" || tag == "f") {
      const auto bytes = take_number<std::uint64_t>(value);
      auto path = unescape_path(value);
      if (!bytes || !path || path->empty())
        return invalid(*line);
      snapshot.entries.push_back(
          {std::move(*path), *bytes, tag == "f", tag == "p"});
    } else {
      return invalid(*line);
    }
  }
  if (!text.empty())
    return std::unexpected("Disk snapshot is truncated");
  std::ranges::sort(snapshot.entries, {}, &DiskEntry::path);
  return snapshot;
}

DiskReport compare_snapshots(const DiskSnapshot *before,
                             const DiskSnapshot &after,
                             const DiskReportLimits &limits)
{
  DiskReport report;
  report.after_time = after.time;
  report.free_after = after.volume_free;
  report.volume_total = after.volume_total;
  if (before) {
    report.before_time = before->time;
    report.free_before = before->volume_free;
    report.volume_compared =
        before->volume_total != 0 && after.volume_total != 0;
  }

  auto merged = merge_entries(before, after);
  const auto excluded = excluded_paths(before, after);
  report.unreadable.assign(excluded.begin(), excluded.end());
  for (const auto path : excluded)
    exclude(merged, path);

  // Apply the limits. A row keeps its parents, so that the tree stays
  // connected.
  std::vector<DiskRow> all;
  all.reserve(merged.size());
  std::unordered_map<std::string_view, std::size_t> index_of;
  for (const auto &[path, row] : merged) {
    if (!is_under_any(path, after.roots))
      continue; // a folder the user has stopped scanning
    const bool compared = before && is_under_any(path, before->roots);
    index_of[path] = all.size();
    all.push_back({std::string(path), compared ? row.before : row.after,
                   row.after, std::nullopt, row.is_file, row.is_project,
                   compared});
  }

  std::vector<bool> keep(all.size());
  const auto limit = static_cast<std::int64_t>(limits.min_change);
  for (std::size_t i = all.size(); i-- > 0;) {
    const auto &row = all[i];
    const auto change = row.change();
    if (std::max(row.before, row.after) >= limits.min_size || change >= limit ||
        change <= -limit)
      keep[i] = true;
    const auto parent = index_of.find(parent_of(row.path));
    if (parent != index_of.end()) {
      all[i].parent = parent->second;
      if (keep[i])
        keep[parent->second] = true;
    }
  }

  // The tops of the compared rows add up to the whole change, with nothing
  // counted twice. This comes before the limits, because a change too small for
  // a row still counts toward the total.
  for (const auto &row : all)
    if (row.compared && (!row.parent || !all[*row.parent].compared))
      report.scanned_change += row.change();

  std::vector<std::size_t> new_index(all.size());
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (!keep[i])
      continue;
    new_index[i] = report.rows.size();
    auto row = std::move(all[i]);
    if (row.parent)
      row.parent = new_index[*row.parent];
    report.rows.push_back(std::move(row));
  }

  std::vector<std::vector<std::size_t>> children(report.rows.size());
  std::vector<std::size_t> tops;
  for (std::size_t i = 0; i < report.rows.size(); ++i) {
    const auto &row = report.rows[i];
    if (row.parent)
      children[*row.parent].push_back(i);
    if (row.compared && (!row.parent || !report.rows[*row.parent].compared))
      tops.push_back(i);
  }
  for (const auto top : tops)
    find_hotspots(report.rows, children, top, true, limits, report.hotspots);
  std::ranges::stable_sort(report.hotspots, std::greater{}, [&](std::size_t i) {
    return report.rows[i].change();
  });
  if (report.hotspots.size() > limits.max_hotspots)
    report.hotspots.resize(limits.max_hotspots);
  return report;
}

std::optional<std::int64_t>
choose_baseline(const std::vector<std::int64_t> &times,
                std::int64_t since_seconds)
{
  if (times.size() < 2)
    return std::nullopt;
  if (since_seconds <= 0)
    return times[times.size() - 2];
  const auto cutoff = times.back() - since_seconds;
  const auto after_cutoff = std::ranges::upper_bound(times, cutoff);
  return after_cutoff == times.begin() ? times.front()
                                       : *std::prev(after_cutoff);
}

std::vector<std::int64_t> expired_scans(const std::vector<std::int64_t> &times,
                                        std::int64_t now)
{
  std::vector<std::int64_t> expired;
  std::set<std::int64_t> days_seen;
  // Newest first, so that the first scan seen for a day is the one kept.
  for (auto it = times.rbegin(); it != times.rend(); ++it) {
    const auto time = *it;
    const auto age_days = (now - time) / seconds_per_day;
    if (it == times.rbegin() || age_days < keep_everything_days)
      continue;
    if (age_days >= keep_daily_days ||
        !days_seen.insert(time / seconds_per_day).second)
      expired.push_back(time);
  }
  return expired;
}

} // namespace hm
