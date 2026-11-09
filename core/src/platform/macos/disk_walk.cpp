#include "hm/disk_walk.hpp"

#include <fts.h>
#include <pthread/qos.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <set>
#include <utility>

namespace hm {
namespace {

using FileId = std::pair<dev_t, ino_t>;

std::uint64_t allocated_bytes(const struct stat &info)
{
  return static_cast<std::uint64_t>(info.st_blocks) *
         512; // POSIX fixes the block size
}

void walk_root(const std::string &root, DiskWalkVisitor &visitor,
               const std::stop_token &stop, std::set<FileId> &linked_files)
{
  auto path = root;
  std::array<char *, 2> paths{path.data(), nullptr};
  FTS *walk =
      fts_open(paths.data(), FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr);
  if (!walk) {
    visitor.unreadable(root);
    return;
  }

  while (!stop.stop_requested()) {
    const FTSENT *entry = fts_read(walk);
    if (!entry)
      break;
    switch (entry->fts_info) {
    case FTS_D:
      visitor.enter_directory(entry->fts_path);
      break;
    case FTS_DP:
      visitor.leave_directory();
      break;
    case FTS_DNR:
      // Follows the FTS_D for this directory, and takes the place of its
      // FTS_DP.
      visitor.unreadable(entry->fts_path);
      visitor.leave_directory();
      break;
    case FTS_ERR:
    case FTS_NS:
      visitor.unreadable(entry->fts_path);
      break;
    case FTS_DC:
      break; // a cycle, which cannot happen without following links
    default: {
      const auto &info = *entry->fts_statp;
      const bool repeat =
          info.st_nlink > 1 &&
          !linked_files.emplace(info.st_dev, info.st_ino).second;
      visitor.file(entry->fts_path, repeat ? 0 : allocated_bytes(info));
    }
    }
  }
  fts_close(walk);
}

std::string home_directory()
{
  // Read while tracking starts, and nothing here sets it.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char *home = std::getenv("HOME"); home && *home)
    return home;
  const passwd *user =
      getpwuid(getuid()); // NOLINT(concurrency-mt-unsafe): as above
  return user && user->pw_dir ? user->pw_dir : "";
}

bool is_directory(const std::string &path)
{
  struct stat info{};
  return !path.empty() && stat(path.c_str(), &info) == 0 &&
         S_ISDIR(info.st_mode);
}

} // namespace

void walk_disk(const std::vector<std::string> &roots, DiskWalkVisitor &visitor,
               const std::stop_token &stop)
{
  std::set<FileId>
      linked_files; // shared, so that a link across two roots counts once
  for (const auto &root : roots) {
    if (stop.stop_requested())
      return;
    walk_root(root, visitor, stop, linked_files);
  }
}

std::vector<std::string> default_disk_roots()
{
  std::vector<std::string> roots;
  for (const auto &path :
       {home_directory(), std::string("/Applications"), std::string("/Library"),
        std::string("/opt"), std::string("/usr/local")})
    if (is_directory(path))
      roots.push_back(path);
  return roots;
}

std::string default_disk_history_dir()
{
  return home_directory() +
         "/Library/Application Support/HuginMunin/DiskHistory";
}

void prepare_disk_scan_thread()
{
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  // Listing a folder held in cloud storage would otherwise download it. The
  // folder is reported as unreadable instead.
  setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD,
                 IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
}

} // namespace hm
