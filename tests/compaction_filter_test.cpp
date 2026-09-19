#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

Bytes bytes(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-compaction-filter";
  std::filesystem::remove_all(path);
  auto db = DB::Open(path);
  db->Put("drop/key", "old");
  db->Put("keep/key", "kept");
  auto reader = db->NewTransaction();
  db->Put("drop/key", "new");
  db->AddCompactionFilter(bytes("drop/"));
  db->ForceFullCompaction();
  assert(as_string(*db->Get("drop/key")) == "new");
  assert(as_string(*db->Get("keep/key")) == "kept");
  reader.reset();
  db->ForceFullCompaction();
  assert(!db->Get("drop/key"));
  assert(as_string(*db->Get("keep/key")) == "kept");
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "Compaction filter tests passed\n";
}
