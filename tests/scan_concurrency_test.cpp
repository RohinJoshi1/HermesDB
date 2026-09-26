#include "hermesdb/db.hpp"

#include <atomic>
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
           ("hermesdb-scan-conc-" +
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
  options.target_sst_size = 2048;
  options.enable_wal = false;
  auto db = DB::Open(directory.path, options);

  constexpr int kKeys = 400;
  for (int i = 0; i < kKeys; ++i) {
    db->Put("k" + std::to_string(i), "v0");
  }

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < 80 && !stop.load(); ++i) {
        db->Put("k" + std::to_string((t * 80 + i) % kKeys),
                "v" + std::to_string(i));
        if (i % 20 == 0) db->ForceFreezeMemTable();
        if (i % 40 == 0) db->ForceFlush();
      }
    });
  }
  workers.emplace_back([&] {
    for (int i = 0; i < 60; ++i) {
      std::string previous;
      int count = 0;
      for (auto it = db->Scan(); it.valid(); it.next()) {
        const auto key = as_string(it.key());
        if (!previous.empty() && key <= previous) ++errors;
        previous = key;
        ++count;
      }
      if (count == 0) ++errors;
    }
    stop.store(true);
  });
  for (auto& worker : workers) worker.join();
  assert(errors.load() == 0);
  std::cout << "Scan concurrency tests passed\n";
}
