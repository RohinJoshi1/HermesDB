#include "hermesdb/db.hpp"
#include "hermesdb/iterator.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace hermesdb;

namespace {

Bytes bytes(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}

struct TemporaryDirectory {
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-iterator-" +
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

void test_merge_priority_and_range_iterator() {
  std::vector<IteratorPtr> children;
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), bytes("new-a")},
                            {InternalKey("c"), bytes("new-c")}}));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<KeyValue>{{InternalKey("a"), bytes("old-a")},
                            {InternalKey("b"), bytes("old-b")}}));
  auto merged = std::make_unique<MergeIterator>(std::move(children));
  RangeIterator range(std::move(merged), InternalKey("a"), false,
                      InternalKey("c"), true);
  assert(range.valid() && as_string(range.key().user_key()) == "b");
  assert(as_string(range.value()) == "old-b");
  range.next();
  assert(range.valid() && as_string(range.key().user_key()) == "c");
  range.next();
  assert(!range.valid());
}

void test_db_scan_sources_tombstones_and_bounds() {
  TemporaryDirectory directory;
  auto db = DB::Open(directory.path);
  db->Put("a", "old-a");
  db->Put("b", "old-b");
  db->Put("d", "old-d");
  db->ForceFreezeMemTable();
  db->Put("a", "new-a");
  db->Delete("b");
  db->Put("c", "new-c");

  auto scan = db->Scan(KeyBound::Excluded("a"), KeyBound::Included("d"));
  assert(scan.valid() && as_string(scan.key()) == "c");
  assert(as_string(scan.value()) == "new-c");
  scan.next();
  assert(scan.valid() && as_string(scan.key()) == "d");
  scan.next();
  assert(!scan.valid());

  auto all = db->Scan();
  assert(all.valid() && as_string(all.key()) == "a");
  assert(as_string(all.value()) == "new-a");
  all.next();
  assert(all.valid() && as_string(all.key()) == "c");
  all.next();
  assert(all.valid() && as_string(all.key()) == "d");
  all.next();
  assert(!all.valid());
}

}  // namespace

int main() {
  test_merge_priority_and_range_iterator();
  test_db_scan_sources_tombstones_and_bounds();
  std::cout << "Iterator tests passed\n";
}
