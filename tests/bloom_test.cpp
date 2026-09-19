#include "hermesdb/table.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace hermesdb;

int main() {
  BlockBuilder block_builder(256);
  assert(block_builder.add(InternalKey("prefix-apple"), as_bytes("red")));
  assert(block_builder.add(InternalKey("prefix-apricot"), as_bytes("orange")));
  auto block = block_builder.finish();
  assert(read_u16(block->data(), block->offset(0)) == 0);
  assert(read_u16(block->data(), block->offset(1)) >= 9);
  BlockIterator block_iterator(Block::decode(block->encode()));
  block_iterator.next();
  assert(as_string(block_iterator.key().user_key()) == "prefix-apricot");

  const std::vector<std::uint32_t> hashes{
      checksum(as_bytes("alpha")), checksum(as_bytes("beta")),
      checksum(as_bytes("gamma"))};
  auto bloom = BloomFilter::Build(
      hashes, BloomFilter::BitsPerKey(hashes.size(), 0.01));
  auto decoded = BloomFilter::Decode(bloom.Encode());
  for (const auto hash : hashes) assert(decoded.MayContain(hash));

  TableBuilder table_builder(64);
  table_builder.add(InternalKey("alpha"), as_bytes("one"));
  table_builder.add(InternalKey("beta"), as_bytes("two"));
  table_builder.add(InternalKey("gamma"), as_bytes("three"));
  auto table = Table::open(table_builder.finish());
  assert(table->may_contain(as_bytes("alpha")));
  assert(table->may_contain(as_bytes("beta")));
  assert(table->may_contain(as_bytes("gamma")));
  assert(as_string(*table->get(as_bytes("gamma"))) == "three");

  std::cout << "Bloom filter tests passed\n";
}
