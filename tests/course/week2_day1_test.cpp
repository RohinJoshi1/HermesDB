#include "tiny_lsm/tiny_lsm.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace tiny_lsm;

namespace {
struct TempDir {
  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("tiny-lsm-w2d1-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

std::string text(const std::optional<Bytes>& value) {
  assert(value);
  return as_string(*value);
}
}  // namespace

int main() {
  TempDir directory;
  LsmStorageOptions options;
  options.target_sst_size = 1U << 20U;
  auto db = MiniLsm::Open(directory.path, options);

  db->Put("a", "old-a");
  db->Put("b", "old-b");
  db->ForceFlush();
  db->Put("a", "new-a");
  db->Delete("b");
  db->Put("c", "new-c");
  db->ForceFlush();
  db->ForceFullCompaction();

  assert(text(db->Get("a")) == "new-a");
  assert(!db->Get("b"));
  assert(text(db->Get("c")) == "new-c");
  assert(db->DumpStructure().find("l0_tables=0") != std::string::npos);
  assert(db->DumpStructure().find("l1_tables=1") != std::string::npos);

  db->Put("a", "newest");
  db->ForceFlush();
  assert(text(db->Get("a")) == "newest");
  assert(db->DumpStructure().find("l0_tables=1") != std::string::npos);
  db->Close();

  TempDir background;
  LsmStorageOptions background_options;
  background_options.num_memtable_limit = 1;
  background_options.target_sst_size = 1U << 20U;
  auto background_db = MiniLsm::Open(background.path, background_options);
  background_db->Put("k", "v");
  background_db->ForceFreezeMemTable();
  const auto sst = background.path / "0.sst";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(sst) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(std::filesystem::exists(sst));
  assert(as_string(*background_db->Get("k")) == "v");
  background_db->Close();

  std::cout << "Week 2 Day 1 tests passed\n";
}
