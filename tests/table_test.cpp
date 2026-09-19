#include "hermesdb/table.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace hermesdb;

int main() {
  TableBuilder builder(48);
  for (int index = 0; index < 20; ++index) {
    const auto key = "key-" + std::to_string(100 + index);
    const auto value = "value-" + std::to_string(index);
    builder.add(InternalKey(key), as_bytes(value));
  }
  auto table = Table::open(builder.finish());
  assert(table->num_blocks() > 1);

  auto iterator = table->iter_from(InternalKey("key-113"));
  assert(iterator->valid());
  assert(as_string(iterator->key().user_key()) == "key-113");
  assert(as_string(iterator->value()) == "value-13");
  iterator = table->iter_from(InternalKey("key-113x"));
  assert(iterator->valid());
  assert(as_string(iterator->key().user_key()) == "key-114");
  iterator = table->iter_from(InternalKey("zzzz"));
  assert(!iterator->valid());

  BlockCache cache(1);
  auto first = table->read_block_cached(0, 7, cache);
  assert(table->read_block_cached(0, 7, cache) == first);
  static_cast<void>(table->read_block_cached(1, 7, cache));
  assert(!cache.Get(7, 0));

  std::cout << "SST tests passed\n";
}
