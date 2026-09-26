#include "hermesdb/block.hpp"
#include "hermesdb/db.hpp"
#include "hermesdb/iterator.hpp"
#include "hermesdb/memtable.hpp"
#include "hermesdb/table.hpp"
#include "hermesdb/transaction.hpp"

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
}

}  // namespace

int main() {
  test_memtable_and_block_seek();
  test_merge_source_priority();
  test_db_scan_oracle();
  test_flush_and_transaction_overlay();
  test_segmented_key_view();
  test_multi_block_table_scan();
  std::cout << "Scan tests passed\n";
}
