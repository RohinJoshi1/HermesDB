#include "tiny_lsm/iterator.hpp"
#include "tiny_lsm/table.hpp"

#include <cassert>
#include <iostream>

using namespace tiny_lsm;

namespace {
Bytes owned(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}
}  // namespace

int main() {
  TableBuilder builder(64);
  builder.add(as_bytes("a"), as_bytes("sst-a"));
  builder.add(as_bytes("b"), ByteView{});  // persisted tombstone
  builder.add(as_bytes("c"), as_bytes("sst-c"));
  auto table = Table::open(builder.finish());
  assert(as_string(*table->get(as_bytes("a"))) == "sst-a");
  assert(table->get(as_bytes("b")).has_value());
  assert(table->get(as_bytes("b"))->empty());
  assert(!table->get(as_bytes("missing")));

  std::vector<IteratorPtr> sources;
  sources.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{owned("a"), owned("mutable-a")},
                            {owned("b"), Bytes{}}}));
  sources.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{owned("a"), owned("immutable-a")}}));
  sources.push_back(table->iter());
  MergeIterator merged(std::move(sources));
  assert(merged.valid() && as_string(merged.key()) == "a");
  assert(as_string(merged.value()) == "mutable-a");
  merged.next();
  assert(merged.valid() && as_string(merged.key()) == "b");
  assert(merged.value().empty());
  merged.next();
  assert(merged.valid() && as_string(merged.key()) == "c");
  merged.next();
  assert(!merged.valid());

  std::cout << "Week 1 Day 5 tests passed\n";
}
