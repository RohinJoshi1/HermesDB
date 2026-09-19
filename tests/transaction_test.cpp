#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "hermesdb-transaction";
  std::filesystem::remove_all(path);
  Options options;
  options.enable_wal = true;
  auto db = DB::Open(path, options);
  db->Put("a", "old");
  auto first = db->NewTransaction();
  first->Put("a", "local");
  first->Put("b", "new");
  assert(as_string(*first->Get("a")) == "local");
  assert(!db->Get("b"));
  first->Commit();
  assert(as_string(*db->Get("a")) == "local");
  assert(as_string(*db->Get("b")) == "new");

  auto loser = db->NewTransaction();
  auto winner = db->NewTransaction();
  winner->Put("a", "winner");
  winner->Commit();
  loser->Put("a", "loser");
  bool conflicted = false;
  try {
    loser->Commit();
  } catch (const Error&) {
    conflicted = true;
  }
  assert(conflicted);
  db->Sync();
  db->Close();
  db = DB::Open(path, options);
  assert(as_string(*db->Get("a")) == "winner");
  assert(as_string(*db->Get("b")) == "new");
  db->Close();
  std::filesystem::remove_all(path);
  std::cout << "Transaction tests passed\n";
}
