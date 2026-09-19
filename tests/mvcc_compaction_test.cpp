#include "hermesdb/db.hpp"
#include "hermesdb/memtable.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

Bytes bytes(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}

int main() {
  MemTable versions;
  versions.put("k", 3, as_bytes("v3"));
  versions.put("k", 2, as_bytes("v2"));
  versions.put("k", 1, as_bytes("v1"));
  assert(versions.entries().size() == 3);

  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-mvcc-compaction";
  std::filesystem::remove_all(path);
  auto db = DB::Open(path);
  const std::vector<WriteBatchRecord> batch{
      PutRecord{bytes("a"), bytes("one")},
      PutRecord{bytes("b"), bytes("two")}};
  db->WriteBatch(batch);
  db->Put("a", "new");
  db->ForceFlush();
  db->ForceFullCompaction();
  assert(as_string(*db->Get("a")) == "new");
  assert(as_string(*db->Get("b")) == "two");
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "MVCC compaction tests passed\n";
}
