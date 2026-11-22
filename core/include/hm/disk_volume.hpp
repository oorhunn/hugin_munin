// Mounted filesystem identity and capacity, implemented by each OS backend.
#pragma once

#include "hm/collectors.hpp"
#include <string>
#include <vector>

namespace hm {
struct DiskVolume {
  std::string id; // filesystem UUID; empty identifies the legacy internal disk
  std::string name;
  std::string mount_path;
  std::string session; // mount instance, used to reject removal during a walk
  std::uint64_t total = 0, free = 0;
};

// Only local external physical volumes with stable identity are eligible.
Collected<std::vector<DiskVolume>> mounted_disk_volumes();
// Resolves symlinks and rejects unsupported filesystems rather than guessing.
Collected<DiskVolume> disk_volume_for_path(const std::string &path);
} // namespace hm
