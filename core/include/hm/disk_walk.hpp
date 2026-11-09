// Platform boundary for the disk tracker: each supported OS implements these
// under core/src/platform/<os>/.
#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace hm {

// Receives a walk in depth-first order. Every enter_directory is matched by a
// leave_directory unless the walk is stopped early.
class DiskWalkVisitor {
public:
  virtual ~DiskWalkVisitor() = default;
  virtual void enter_directory(const std::string &path) = 0;
  // `bytes` is the space allocated on disk, not the logical length. It is zero
  // for a further hard link to a file already reported.
  virtual void file(const std::string &path, std::uint64_t bytes) = 0;
  virtual void leave_directory() = 0;
  // Something the walk could not read, so its size is unknown rather than zero.
  virtual void unreadable(const std::string &path) = 0;
};

// Walks each root without following symlinks or crossing into another volume.
// Never downloads files held in cloud storage.
void walk_disk(const std::vector<std::string> &roots, DiskWalkVisitor &visitor,
               const std::stop_token &stop);

// The folders to scan until the user chooses others. Only folders that exist
// are returned.
std::vector<std::string> default_disk_roots();

// Where scans are stored between launches.
std::string default_disk_history_dir();

// Makes the calling thread give way to the user's work, since a scan can take
// minutes.
void prepare_disk_scan_thread();

} // namespace hm
