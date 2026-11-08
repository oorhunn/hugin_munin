#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace hm {
// Session-only references. The shelf never copies, moves, or deletes the
// originals.
class FileShelf {
public:
  bool add(const std::string &path)
  {
    const std::filesystem::path candidate(path);
    if (!candidate.is_absolute())
      return false;
    const auto normalized = candidate.lexically_normal().string();
    if (std::ranges::find(paths_, normalized) != paths_.end())
      return false;
    paths_.push_back(normalized);
    return true;
  }
  void remove(const std::string &path)
  {
    std::erase(paths_, path);
  }
  [[nodiscard]] const std::vector<std::string> &paths() const
  {
    return paths_;
  }

private:
  std::vector<std::string> paths_;
};
} // namespace hm
