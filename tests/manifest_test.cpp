#include "hermesdb/hermesdb.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("hermesdb-manifest-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(path);
  Options options;
  options.compaction_options = NoCompactionOptions{};
  {
    auto db = DB::Open(path, options);
    db->Put("a", "old");
    db->Put("gone", "old");
    db->ForceFlush();
    db->Put("a", "new");
    db->Delete("gone");
    db->ForceFlush();
    db->ForceFullCompaction();
    db->Close();
  }
  assert(std::filesystem::exists(path / "MANIFEST"));
  {
    auto db = DB::Open(path, options);
    assert(as_string(*db->Get("a")) == "new");
    assert(!db->Get("gone"));
    db->Put("after", "restart");
    db->ForceFlush();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    assert(as_string(*db->Get("after")) == "restart");
    db->Close();
  }
  std::filesystem::remove_all(path);
  std::cout << "Manifest tests passed\n";
}
