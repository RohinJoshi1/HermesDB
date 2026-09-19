#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-serializable";
  std::filesystem::remove_all(path);
  Options options;
  options.serializable = true;
  auto db = DB::Open(path, options);
  db->Put("a", "on");
  db->Put("b", "on");
  auto first = db->NewTransaction();
  auto second = db->NewTransaction();
  assert(first->Get("b"));
  assert(second->Get("a"));
  first->Put("a", "off");
  second->Put("b", "off");
  first->Commit();
  bool rejected = false;
  try {
    second->Commit();
  } catch (const Error&) {
    rejected = true;
  }
  assert(rejected);

  auto missing_reader = db->NewTransaction();
  assert(!missing_reader->Get("missing"));
  db->Put("missing", "inserted");
  missing_reader->Put("dependent", "value");
  rejected = false;
  try {
    missing_reader->Commit();
  } catch (const Error&) {
    rejected = true;
  }
  assert(rejected);
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "Serializable transaction tests passed\n";
}
