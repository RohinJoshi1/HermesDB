#include "hermesdb/hermesdb.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace hermesdb;

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("hermesdb-simple-leveled-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(path);
  Options options;
  options.target_sst_size = 1U << 20U;
  options.compaction_options = SimpleLeveledCompactionOptions{
      .size_ratio_percent = 200,
      .level0_file_num_compaction_trigger = 2,
      .max_levels = 3};
  auto db = DB::Open(path, options);
  db->Put("key", "old");
  db->ForceFlush();
  db->Put("key", "new");
  db->Put("other", "value");
  db->ForceFlush();

  for (int attempt = 0; attempt != 100 &&
                        db->DumpStructure().find("l0_tables=0") ==
                            std::string::npos;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(db->DumpStructure().find("l0_tables=0") != std::string::npos);
  assert(as_string(*db->Get("key")) == "new");
  assert(as_string(*db->Get("other")) == "value");
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "Simple leveled compaction tests passed\n";
}
