#include "tiny_lsm/block.hpp"

#include <cassert>
#include <iostream>

using namespace tiny_lsm;

int main() {
  BlockBuilder builder(128);
  assert(builder.add(as_bytes("apple"), as_bytes("red")));
  assert(builder.add(as_bytes("banana"), as_bytes("yellow")));

  const auto encoded = builder.finish()->encode();
  const auto block = Block::decode(encoded);
  assert(block->size() == 2);
  assert(read_u16(block->data(), 0) == 0);  // first-key prefix overlap
  assert(read_u16(block->data(), 2) == 5);  // remaining "apple" bytes

  BlockIterator iterator(block);
  assert(iterator.valid());
  assert(as_string(iterator.key()) == "apple");
  assert(as_string(iterator.value()) == "red");

  iterator.seek(as_bytes("apricot"));
  assert(iterator.valid());
  assert(as_string(iterator.key()) == "banana");
  assert(as_string(iterator.value()) == "yellow");
  iterator.next();
  assert(!iterator.valid());

  bool rejected = false;
  try {
    auto corrupt = encoded;
    corrupt[corrupt.size() - 4] = 0xff;
    static_cast<void>(Block::decode(std::move(corrupt)));
  } catch (const Error&) {
    rejected = true;
  }
  assert(rejected);

  BlockBuilder tiny(12);
  assert(tiny.add(as_bytes("oversized"), as_bytes("allowed")));
  assert(!tiny.add(as_bytes("next"), as_bytes("rejected")));

  std::cout << "Week 1 Day 3 tests passed\n";
}
