#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-snapshot";
  std::filesystem::remove_all(path);
  Options options;
  options.enable_wal = true;
  {
    auto db = DB::Open(path, options);
    db->Put("key", "old");
    auto snapshot = db->NewTransaction();
    const auto read_timestamp = snapshot->read_timestamp();
    db->Put("key", "new");
    db->Delete("other");
    assert(as_string(*snapshot->Get("key")) == "old");
    assert(snapshot->read_timestamp() == read_timestamp);
    assert(as_string(*db->Get("key")) == "new");
    db->Sync();
    snapshot.reset();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    auto recovered = db->NewTransaction();
    assert(recovered->read_timestamp() >= 3);
    assert(as_string(*recovered->Get("key")) == "new");
    db->Close();
  }
  std::filesystem::remove_all(path);
  std::cout << "Snapshot tests passed\n";
}
