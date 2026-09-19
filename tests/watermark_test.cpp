#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-watermark";
  std::filesystem::remove_all(path);
  auto db = DB::Open(path);
  db->Put("key", "v1");
  auto oldest = db->NewTransaction();
  auto duplicate_reader = db->NewTransaction();
  db->Put("key", "v2");
  db->Put("key", "v3");
  db->ForceFullCompaction();
  assert(as_string(*oldest->Get("key")) == "v1");
  assert(db->DumpStructure().find("versions=3") != std::string::npos);
  oldest.reset();
  db->ForceFullCompaction();
  assert(as_string(*duplicate_reader->Get("key")) == "v1");
  assert(db->DumpStructure().find("versions=3") != std::string::npos);
  duplicate_reader.reset();
  db->ForceFullCompaction();
  assert(as_string(*db->Get("key")) == "v3");
  assert(db->DumpStructure().find("versions=1") != std::string::npos);
  assert(db->DumpStructure().find("active_snapshots=0") != std::string::npos);
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "Watermark tests passed\n";
}
