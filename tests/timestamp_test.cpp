#include "hermesdb/hermesdb.hpp"
#include "hermesdb/key.hpp"
#include "hermesdb/memtable.hpp"
#include "hermesdb/table.hpp"

#include <cassert>
#include <iostream>

using namespace hermesdb;

int main() {
  const InternalKey newest("key", 9);
  const InternalKey older("key", 2);
  assert(newest < older);
  assert(InternalKey("a", 1) < newest);
  assert(InternalKey::decode(newest.encode()) == newest);

  MemTable memtable;
  memtable.put("key", 9, as_bytes("new"));
  memtable.put("key", 2, as_bytes("old"));
  assert(as_string(*memtable.get("key", 9)) == "new");
  assert(as_string(*memtable.get("key", 8)) == "old");

  TableBuilder builder(128);
  builder.add(newest, as_bytes("new"));
  builder.add(older, as_bytes("old"));
  auto table = Table::open(builder.finish());
  auto iterator = table->iter();
  assert(iterator->key() == newest);
  iterator->next();
  assert(iterator->key() == older);
  std::cout << "Timestamp tests passed\n";
}
