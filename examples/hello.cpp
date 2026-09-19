#include "hermesdb/db.hpp"

#include <iostream>
#include <stdexcept>

int main() {
  try {
    hermesdb::Options options;
    options.enable_wal = true;
    auto db = hermesdb::DB::Open("hello.db", options);
    db->Put("greeting", "hello, HermesDB");
    db->Sync();
    if (const auto value = db->Get("greeting")) {
      std::cout << hermesdb::as_string(*value) << '\n';
    }
    db->Close();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
  return 0;
}
