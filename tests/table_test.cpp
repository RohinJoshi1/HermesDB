#include "hermesdb/table.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
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
  assert(cache.hits() >= 1);
  assert(cache.misses() >= 1);

  BlockCache protected_cache(1);
  auto pinned = table->read_block_cached(0, 9, protected_cache);
  static_cast<void>(table->read_block_for_scan(1, &protected_cache, 9));
  assert(protected_cache.Get(9, 0) == pinned);

  BlockCache prefetch_cache(16);
  const auto head = table->read_block_for_scan(0, &prefetch_cache, 3);
  assert(prefetch_cache.Get(3, 0) == head);
  assert(prefetch_cache.Contains(3, 1));
  if (table->num_blocks() > 2) assert(prefetch_cache.Contains(3, 2));

  const Bytes encoded(table->bytes().begin(), table->bytes().end());
  const auto path =
      std::filesystem::temp_directory_path() /
      ("hermesdb-sst-pread-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".sst");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(encoded.data()),
              static_cast<std::streamsize>(encoded.size()));
    assert(out);
  }
  auto file_table = Table::open(path);
  assert(file_table->file_size() == encoded.size());
  assert(file_table->num_blocks() == table->num_blocks());
  assert(as_string(*file_table->get(as_bytes("key-113"))) == "value-13");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  std::cout << "SST tests passed\n";
}
