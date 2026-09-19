#include "hermesdb/iterator.hpp"
#include "hermesdb/table.hpp"

#include <cassert>
#include <iostream>

using namespace hermesdb;

namespace {
Bytes owned(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}
}  // namespace

int main() {
  TableBuilder builder(64);
  builder.add(InternalKey("a"), as_bytes("sst-a"));
  builder.add(InternalKey("b"), ByteView{});  // persisted tombstone
  builder.add(InternalKey("c"), as_bytes("sst-c"));
  auto table = Table::open(builder.finish());
  assert(as_string(*table->get(as_bytes("a"))) == "sst-a");
  assert(table->get(as_bytes("b")).has_value());
  assert(table->get(as_bytes("b"))->empty());
  assert(!table->get(as_bytes("missing")));

  std::vector<IteratorPtr> sources;
  sources.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), owned("mutable-a")},
                            {InternalKey("b"), Bytes{}}}));
  sources.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), owned("immutable-a")}}));
  sources.push_back(table->iter());
  MergeIterator merged(std::move(sources));
  assert(merged.valid() && as_string(merged.key().user_key()) == "a");
  assert(as_string(merged.value()) == "mutable-a");
  merged.next();
  assert(merged.valid() && as_string(merged.key().user_key()) == "b");
  assert(merged.value().empty());
  merged.next();
  assert(merged.valid() && as_string(merged.key().user_key()) == "c");
  merged.next();
  assert(!merged.valid());

  std::cout << "Read path tests passed\n";
}
