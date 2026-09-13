#include "tiny_lsm/table.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace tiny_lsm;

int main() {
  TableBuilder builder(48);
  for (int index = 0; index < 20; ++index) {
    const auto key = "key-" + std::to_string(100 + index);
    const auto value = "value-" + std::to_string(index);
    builder.add(as_bytes(key), as_bytes(value));
  }
  auto table = Table::open(builder.finish());
  assert(table->num_blocks() > 1);

  auto iterator = table->iter_from(as_bytes("key-113"));
  assert(iterator->valid());
  assert(as_string(iterator->key()) == "key-113");
  assert(as_string(iterator->value()) == "value-13");
  iterator = table->iter_from(as_bytes("key-113x"));
  assert(iterator->valid());
  assert(as_string(iterator->key()) == "key-114");
  iterator = table->iter_from(as_bytes("zzzz"));
  assert(!iterator->valid());

  BlockCache cache(1);
  auto first = table->read_block_cached(0, 7, cache);
  assert(table->read_block_cached(0, 7, cache) == first);
  static_cast<void>(table->read_block_cached(1, 7, cache));
  assert(!cache.Get(7, 0));

  std::cout << "Week 1 Day 4 tests passed\n";
}
