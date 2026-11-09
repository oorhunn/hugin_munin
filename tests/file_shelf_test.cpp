#include "check.hpp"
#include "hm_bridge.h"

#include <string>

int main()
{
  auto *shelf = hm_file_shelf_create();
  require(shelf != nullptr, "creates shelf");
  require(!hm_file_shelf_add(shelf, "relative/file"), "rejects relative paths");
  require(hm_file_shelf_add(shelf, "/tmp/folder/../document"),
          "adds document reference");
  require(!hm_file_shelf_add(shelf, "/tmp/document"),
          "deduplicates normalized paths");
  require(hm_file_shelf_add(shelf, "/tmp/folder"), "adds folder reference");
  require(hm_file_shelf_count(shelf) == 2, "holds multiple references");
  require(std::string(hm_file_shelf_path(shelf, 0)) == "/tmp/document",
          "returns original path");
  hm_file_shelf_remove(shelf, "/tmp/document");
  require(hm_file_shelf_count(shelf) == 1, "removes reference");
  require(hm_file_shelf_path(shelf, 1) == nullptr, "bounds checks path access");
  hm_file_shelf_destroy(shelf);
}
