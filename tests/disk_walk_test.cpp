// The platform walker, against a real folder tree.
#include "check.hpp"
#include "hm/disk_snapshot.hpp"
#include "hm/disk_walk.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t mb = hm::mebibyte;

struct Recorder final : hm::DiskWalkVisitor {
  std::vector<std::string> directories, unreadable_paths;
  std::map<std::string, std::uint64_t> files;
  int depth = 0, max_depth = 0;

  void enter_directory(const std::string &path) override
  {
    directories.push_back(path);
    max_depth = std::max(max_depth, ++depth);
  }
  void file(const std::string &path, std::uint64_t bytes) override
  {
    files[path] = bytes;
  }
  void leave_directory() override
  {
    --depth;
  }
  void unreadable(const std::string &path) override
  {
    unreadable_paths.push_back(path);
  }
};

void write_file(const fs::path &path, std::uint64_t bytes)
{
  std::ofstream file(path, std::ios::binary);
  const std::string chunk(mb, 'x');
  for (std::uint64_t i = 0; i < bytes / mb; ++i)
    file << chunk;
}

bool contains(const std::vector<std::string> &list, const fs::path &path)
{
  return std::ranges::find(list, path.string()) != list.end();
}

// Resolves the temporary folder, because /tmp and /var are symlinks on macOS.
fs::path make_root()
{
  const auto root = fs::canonical(fs::temp_directory_path()) /
                    ("hm_disk_walk_test_" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

void test_walks_a_tree(const fs::path &root)
{
  fs::create_directories(root / "a/b");
  write_file(root / "a/b/two", 2 * mb);
  write_file(root / "a/one", 1 * mb);
  fs::create_hard_link(root / "a/one", root / "a/one_again");
  fs::create_directory_symlink(root / "a", root / "link_to_a");

  Recorder recorder;
  hm::walk_disk({root.string()}, recorder, {});

  require(recorder.depth == 0, "every directory entered is left");
  require(contains(recorder.directories, root / "a/b"), "folders are visited");
  require(!contains(recorder.directories, root / "link_to_a"),
          "symlinks are not followed");
  require(recorder.files.contains((root / "link_to_a").string()),
          "a symlink counts as a file");
  require(recorder.files[(root / "a/b/two").string()] >= 2 * mb,
          "allocated size is reported");

  const auto first = recorder.files[(root / "a/one").string()];
  const auto second = recorder.files[(root / "a/one_again").string()];
  require(std::min(first, second) == 0 && std::max(first, second) >= 1 * mb,
          "a hard link is counted once");
}

void test_unreadable_folder(const fs::path &root)
{
  if (getuid() == 0)
    return; // root reads everything, so this cannot be arranged

  const auto locked = root / "locked";
  fs::create_directories(locked / "inside");
  write_file(root / "after", 1 * mb);
  chmod(locked.c_str(), 0);

  Recorder recorder;
  hm::walk_disk({root.string()}, recorder, {});
  chmod(locked.c_str(), 0755);

  require(contains(recorder.unreadable_paths, locked),
          "an unreadable folder is reported");
  require(recorder.depth == 0, "and the walk stays balanced after it");
  require(recorder.files.contains((root / "after").string()),
          "and it carries on");
}

void test_missing_root(const fs::path &root)
{
  Recorder recorder;
  const auto missing = root / "does_not_exist";
  hm::walk_disk({missing.string()}, recorder, {});
  require(contains(recorder.unreadable_paths, missing),
          "a missing root is reported");
  require(recorder.directories.empty() && recorder.depth == 0,
          "and nothing else happens");
}

void test_stops_when_asked(const fs::path &root)
{
  std::stop_source source;
  source.request_stop();
  Recorder recorder;
  hm::walk_disk({root.string()}, recorder, source.get_token());
  require(recorder.directories.empty(), "a stopped walk reads nothing");
}

void test_feeds_a_snapshot(const fs::path &root)
{
  fs::create_directories(root / "project/.git");
  write_file(root / "project/data", 12 * mb);

  hm::DiskSnapshotBuilder builder;
  hm::walk_disk({root.string()}, builder, {});
  const auto snapshot = builder.finish();

  const auto entry = std::ranges::find(
      snapshot.entries, (root / "project").string(), &hm::DiskEntry::path);
  require(entry != snapshot.entries.end(), "the real tree makes a snapshot");
  require(entry->is_project && entry->bytes >= 12 * mb,
          "with sizes and projects");
}

void test_defaults()
{
  require(hm::default_disk_history_dir().ends_with("HuginMunin/DiskHistory"),
          "history folder");
  const auto roots = hm::default_disk_roots();
  require(!roots.empty(), "there is something to scan");
  for (const auto &root : roots)
    require(fs::is_directory(root), "default roots exist");
}

} // namespace

int main()
{
  const auto root = make_root();
  test_walks_a_tree(root);
  test_unreadable_folder(root);
  test_missing_root(root);
  test_stops_when_asked(root);
  test_feeds_a_snapshot(root);
  test_defaults();
  fs::remove_all(root);
}
