#include "tiny_lsm/db.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace tiny_lsm;

namespace {
struct TemporaryDirectory {
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("tiny-lsm-week1-day6-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};
}  // namespace

int main() {
  TemporaryDirectory directory;
  LsmStorageOptions options;
  options.block_size = 64;
  options.target_sst_size = 1U << 20U;
  auto db = MiniLsm::Open(directory.path, options);

  db->Put("a", "old-a");
  db->Put("b", "old-b");
  db->ForceFlush();
  assert(std::filesystem::exists(directory.path / "0.sst"));

  db->Put("a", "new-a");
  db->Delete("b");
  db->Put("c", "new-c");
  db->ForceFlush();
  assert(std::filesystem::exists(directory.path / "1.sst"));
  assert(as_string(*db->Get("a")) == "new-a");
  assert(!db->Get("b"));  // newer L0 tombstone hides the older L0 value
  assert(as_string(*db->Get("c")) == "new-c");

  const auto structure = db->DumpStructure();
  assert(structure.find("immutable_memtables=0") != std::string::npos);
  assert(structure.find("l0_tables=2") != std::string::npos);

  auto scan = db->Scan(KeyBound::Included("a"), KeyBound::Excluded("d"));
  assert(scan.valid() && as_string(scan.key()) == "a");
  assert(as_string(scan.value()) == "new-a");
  scan.next();
  assert(scan.valid() && as_string(scan.key()) == "c");
  scan.next();
  assert(!scan.valid());

  std::cout << "Week 1 Day 6 tests passed\n";
}
