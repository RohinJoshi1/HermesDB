#include "hermesdb/hermesdb.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("hermesdb-wal-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(path);
  Options options;
  options.enable_wal = true;
  options.compaction_options = NoCompactionOptions{};
  {
    auto db = DB::Open(path, options);
    db->Put("a", "one");
    db->ForceFreezeMemTable();
    db->Put("b", "two");
    db->Delete("a");
    db->Sync();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    assert(!db->Get("a"));
    assert(as_string(*db->Get("b")) == "two");
    db->Put("c", "three");
    db->ForceFlush();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    assert(as_string(*db->Get("b")) == "two");
    assert(as_string(*db->Get("c")) == "three");
    db->Close();
  }
  std::filesystem::remove_all(path);
  std::cout << "WAL tests passed\n";
}
