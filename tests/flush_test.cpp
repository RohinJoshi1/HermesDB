#include "hermesdb/db.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace hermesdb;

namespace {
struct TemporaryDirectory {
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-flush-" +
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
  Options options;
  options.block_size = 64;
  options.target_sst_size = 1U << 20U;
  auto db = DB::Open(directory.path, options);

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
  const auto m1 = db->Metrics();
  assert(as_string(*db->Get("a")) == "new-a");
  const auto m2 = db->Metrics();
  assert(m2.block_cache_hits > m1.block_cache_hits);
  assert(m1.block_cache_misses > 0);

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

  {
    TemporaryDirectory concurrent;
    Options busy;
    busy.block_size = 64;
    busy.target_sst_size = 4096;
    busy.enable_wal = true;
    busy.compaction_options = NoCompactionOptions{};
    auto busy_db = DB::Open(concurrent.path, busy);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 400;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          const auto key = "k-" + std::to_string(t) + "-" + std::to_string(i);
          busy_db->Put(key, "value");
        }
      });
    }
    for (auto& worker : workers) worker.join();
    busy_db->ForceFlush();
    assert(as_string(*busy_db->Get("k-0-0")) == "value");
    assert(as_string(*busy_db->Get("k-7-399")) == "value");
  }

  std::cout << "Flush tests passed\n";
}
