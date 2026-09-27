#include "hermesdb/block.hpp"
#include "hermesdb/db.hpp"
#include "hermesdb/iterator.hpp"
#include "hermesdb/memtable.hpp"
#include "hermesdb/table.hpp"
#include "hermesdb/transaction.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace hermesdb;

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-scan-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::vector<std::pair<std::string, std::string>> collect(DbIterator iterator) {
  std::vector<std::pair<std::string, std::string>> rows;
  while (iterator.valid()) {
    rows.emplace_back(as_string(iterator.key()), as_string(iterator.value()));
    iterator.next();
  }
  return rows;
}

void test_memtable_and_block_seek() {
  auto table = std::make_shared<MemTable>();
  table->put("a", 2, as_bytes("a2"));
  table->put("a", 1, as_bytes("a1"));
  table->put("c", 3, as_bytes("c3"));
  table->erase("b", 4);
  auto it = MemTable::iter_from(table, InternalKey("a", kMaxTimestamp));
  assert(it->valid() && as_string(it->key().user_key()) == "a");
  assert(it->key().timestamp() == 2);
  it->next();
  assert(it->valid() && it->key().timestamp() == 1);
  it->next();
  assert(it->valid() && as_string(it->key().user_key()) == "b");
  assert(it->value().empty());

  BlockBuilder builder(256);
  assert(builder.add(InternalKey("key-a", 3), as_bytes("va")));
  assert(builder.add(InternalKey("key-b", 2), as_bytes("vb")));
  assert(builder.add(InternalKey("key-c", 1), as_bytes("vc")));
  BlockIterator block(builder.finish());
  block.seek(InternalKey("key-b", kMaxTimestamp));
  assert(block.valid() && as_string(block.key().user_key()) == "key-b");
  block.seek(InternalKey("key-b", 0));
  assert(block.valid() && as_string(block.key().user_key()) == "key-c");
  block.seek(InternalKey("key-z", kMaxTimestamp));
  assert(!block.valid());
}

void test_merge_source_priority() {
  std::vector<IteratorPtr> children;
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), Bytes{'n'}},
                            {InternalKey("c"), Bytes{'n'}}}));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), Bytes{'o'}},
                            {InternalKey("b"), Bytes{'o'}}}));
  RangeIterator range(std::make_unique<MergeIterator>(std::move(children)),
                      InternalKey("a"), false, InternalKey("c"), true);
  assert(range.valid() && as_string(range.key().user_key()) == "b");
  range.next();
  assert(range.valid() && as_string(range.key().user_key()) == "c");
  range.next();
  assert(!range.valid());
}

void test_db_scan_oracle() {
  TemporaryDirectory directory;
  auto db = DB::Open(directory.path);
  db->Put("a", "old-a");
  db->Put("b", "old-b");
  db->Put("d", "old-d");
  db->ForceFreezeMemTable();
  db->Put("a", "new-a");
  db->Delete("b");
  db->Put("c", "new-c");

  assert((collect(db->Scan()) ==
          std::vector<std::pair<std::string, std::string>>{
              {"a", "new-a"}, {"c", "new-c"}, {"d", "old-d"}}));
  assert((collect(db->Scan(KeyBound::Excluded("a"), KeyBound::Included("d"))) ==
          std::vector<std::pair<std::string, std::string>>{
              {"c", "new-c"}, {"d", "old-d"}}));

  const auto before = db->Metrics().scan_keys;
  {
    auto partial = db->Scan();
    assert(partial.valid() && as_string(partial.key()) == "a");
    partial.next();
    assert(as_string(partial.key()) == "c");
  }
  assert(db->Metrics().scan_keys == before + 2);
}

void test_flush_and_transaction_overlay() {
  TemporaryDirectory directory;
  Options options;
  options.block_size = 64;
  options.target_sst_size = 4096;
  auto db = DB::Open(directory.path, options);
  db->Put("a", "a0");
  db->Put("b", "b0");
  db->Put("gone", "present");
  db->ForceFlush();
  db->Put("a", "a1");
  db->Delete("b");

  auto txn = db->NewTransaction();
  db->Put("later", "outside");
  txn->Put("new", "value");
  txn->Delete("gone");
  assert((collect(txn->Scan()) ==
          std::vector<std::pair<std::string, std::string>>{
              {"a", "a1"}, {"new", "value"}}));
  assert((collect(txn->Scan(KeyBound::Included("a"), KeyBound::Excluded("n"))) ==
          std::vector<std::pair<std::string, std::string>>{{"a", "a1"}}));
  txn->Commit();
}

void test_simd_key_compare() {
  const auto a16 = as_bytes("0123456789abcdef");
  const auto b16 = as_bytes("0123456789abcdee");
  const auto c16 = as_bytes("1123456789abcdef");
  assert(compare_bytes(a16, a16) == 0);
  assert(compare_bytes(a16, b16) > 0);
  assert(compare_bytes(b16, a16) < 0);
  assert(compare_bytes(a16, c16) < 0);
  assert(compare_bytes(as_bytes("ab"), as_bytes("abc")) < 0);
  assert(compare_bytes({}, {}) == 0);
  assert(compare_bytes(as_bytes("z"), {}) > 0);

  Bytes long_a(32, 'a');
  Bytes long_b(32, 'a');
  long_b[17] = 'b';
  assert(compare_bytes(long_a, long_b) < 0);

  std::array<ScanRow, 4> rows{};
  Bytes k0(a16.begin(), a16.end());
  Bytes k1(a16.begin(), a16.end());
  Bytes k2(c16.begin(), c16.end());
  rows[0] = {InternalKeyView(k0, 3), {}};
  rows[1] = {InternalKeyView(k1, 2), {}};
  rows[2] = {InternalKeyView(k2, 1), {}};
  assert(leading_same_user({rows.data(), 3}, k0) == 2);
  assert(same_user(InternalKeyView(k0, 1), k0));
}

void test_segmented_key_view() {
  BlockBuilder builder(256);
  assert(builder.add(InternalKey("prefix-aaa", 3), as_bytes("v1")));
  assert(builder.add(InternalKey("prefix-aaa", 2), as_bytes("")));
  assert(builder.add(InternalKey("prefix-aab", 1), as_bytes("v3")));
  BlockIterator block(builder.finish());
  const auto first = block.key_view();
  assert(first.timestamp() == 3);
  assert(same_user(first, as_bytes("prefix-aaa")));
  Bytes assembled;
  first.materialize_user(assembled);
  assert(as_string(assembled) == "prefix-aaa");
  block.skip_current_user();
  assert(block.valid());
  assert(same_user(block.key_view(), as_bytes("prefix-aab")));
  assert(as_string(block.value()) == "v3");

  InternalKeyView left(as_bytes("ab"), as_bytes("cd"), 1);
  InternalKeyView right(as_bytes("abc"), as_bytes("d"), 9);
  assert(same_user(left, right));
  assert(compare_internal(left, right) > 0);
}

void test_multi_block_table_scan() {
  TableBuilder builder(48);
  for (int i = 0; i < 20; ++i) {
    builder.add(InternalKey("k" + std::to_string(100 + i)),
                as_bytes("v" + std::to_string(i)));
  }
  auto table = Table::open(builder.finish());
  assert(table->num_blocks() > 1);
  auto it = table->iter_scan(InternalKey("k105"), InternalKey("k110"));
  std::vector<std::string> keys;
  for (; it->valid(); it->next()) {
    keys.emplace_back(as_string(it->key().user_key()));
  }
  assert((keys == std::vector<std::string>{"k105", "k106", "k107", "k108",
                                           "k109"}));

  std::array<ScanRow, kScanBatch> batch{};
  auto pull_it = table->iter_scan(InternalKey{}, InternalKey("k999"));
  keys.clear();
  std::string previous;
  for (;;) {
    const auto n = pull_it->pull(batch);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      Bytes user;
      batch[i].key.materialize_user(user);
      keys.emplace_back(as_string(user));
      if (!previous.empty()) assert(keys.back() > previous);
      previous = keys.back();
    }
  }
  assert(keys.size() == 20);
}

void test_multi_sst_full_scan() {
  TemporaryDirectory directory;
  Options options;
  options.block_size = 64;
  options.target_sst_size = 2048;
  options.enable_wal = false;
  auto db = DB::Open(directory.path, options);
  for (int i = 0; i < 400; ++i) {
    db->Put("k" + std::to_string(i), "v0");
  }
  db->ForceFreezeMemTable();
  db->ForceFlush();
  std::string previous;
  int count = 0;
  for (auto it = db->Scan(); it.valid(); it.next()) {
    const auto key = as_string(it.key());
    assert(previous.empty() || key > previous);
    previous = key;
    ++count;
  }
  assert(count == 400);
}

}  // namespace

int main() {
  test_memtable_and_block_seek();
  test_merge_source_priority();
  test_db_scan_oracle();
  test_flush_and_transaction_overlay();
  test_simd_key_compare();
  test_segmented_key_view();
  test_multi_block_table_scan();
  test_multi_sst_full_scan();
  std::cout << "Scan tests passed\n";
}
