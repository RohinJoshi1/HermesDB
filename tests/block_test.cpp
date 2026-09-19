#include "hermesdb/block.hpp"

#include <cassert>
#include <iostream>

using namespace hermesdb;

int main() {
  InternalKey key("plain-user-key");
  assert(key.encode().size() == std::string_view("plain-user-key").size() + 8);
  assert(InternalKey::decode(key.encode()) == key);

  BlockBuilder builder(128);
  assert(builder.add(InternalKey("apple"), as_bytes("red")));
  assert(builder.add(InternalKey("banana"), as_bytes("yellow")));
  auto encoded = builder.finish()->encode();
  auto block = Block::decode(encoded);

  BlockIterator iterator(block);
  assert(iterator.valid() && as_string(iterator.key().user_key()) == "apple");
  assert(as_string(iterator.value()) == "red");
  iterator.seek(InternalKey("banana"));
  assert(iterator.valid() && as_string(iterator.value()) == "yellow");
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
  assert(tiny.add(InternalKey("oversized"), as_bytes("allowed")));
  assert(!tiny.add(InternalKey("next"), as_bytes("rejected")));
  std::cout << "Block tests passed\n";
}
