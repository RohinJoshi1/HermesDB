#include "hermesdb/db.hpp"
#include "hermesdb/memtable.hpp"

#include <barrier>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hermesdb;

namespace {

std::string text(const std::optional<Bytes>& value) {
  assert(value.has_value());
  return as_string(*value);
}

struct TemporaryDirectory {
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-memtable-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

std::shared_ptr<DB> open_db(const std::filesystem::path& path,
                                 std::size_t capacity = 1024) {
  Options options;
  options.target_sst_size = capacity;
  return DB::Open(path, options);
}

void test_memtable_overwrite_tombstone_and_order() {
  MemTable memtable;
  assert(memtable.empty());

  memtable.put("b", "two");
  memtable.put("a", "one");
  const auto original_size = memtable.approximate_size();
  memtable.put("a", "updated");

  assert(text(memtable.get("a")) == "updated");
  assert(text(memtable.get("b")) == "two");
  assert(!memtable.get("missing"));
  assert(memtable.approximate_size() == original_size + 4);

  memtable.put(as_bytes("a"), ByteView{});
  const auto tombstone = memtable.get("a");
  assert(tombstone.has_value());
  assert(tombstone->empty());

  const auto entries = memtable.entries();
  assert(entries.size() == 2);
  assert(as_string(entries[0].first.user_key()) == "a");
  assert(as_string(entries[1].first.user_key()) == "b");
}

void test_newest_memtable_and_tombstone_win() {
  TemporaryDirectory directory;
  auto db = open_db(directory.path);

  db->Put("key", "oldest");
  db->ForceFreezeMemTable();
  db->Put("key", "newest");
  db->ForceFreezeMemTable();
  assert(text(db->Get("key")) == "newest");

  db->Delete("key");
  assert(!db->Get("key"));

  const auto structure = db->DumpStructure();
  assert(structure.find("immutable_memtables=2") != std::string::npos);
  assert(structure.find("immutable[0]=1 entries=1") != std::string::npos);
  assert(structure.find("immutable[1]=0 entries=1") != std::string::npos);
}

void test_capacity_freezes_memtable() {
  TemporaryDirectory directory;
  auto db = open_db(directory.path, 4);

  db->Put("aa", "bb");
  const auto structure = db->DumpStructure();
  assert(structure.find("mutable_memtable=1 entries=0") != std::string::npos);
  assert(structure.find("immutable_memtables=1") != std::string::npos);
  assert(structure.find("immutable[0]=0 entries=1") != std::string::npos);
  assert(text(db->Get("aa")) == "bb");
}

void test_concurrent_force_freeze_never_publishes_empty_memtable() {
  TemporaryDirectory directory;
  auto db = open_db(directory.path);
  db->Put("key", "value");

  constexpr std::size_t thread_count = 16;
  std::barrier start(static_cast<std::ptrdiff_t>(thread_count));
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    threads.emplace_back([&] {
      start.arrive_and_wait();
      db->ForceFreezeMemTable();
    });
  }
  for (auto& thread : threads) thread.join();

  const auto structure = db->DumpStructure();
  assert(structure.find("immutable_memtables=1") != std::string::npos);
  assert(structure.find("immutable[0]=0 entries=1") != std::string::npos);
  assert(structure.find("entries=0\nimmutable[") == std::string::npos);
}

void test_concurrent_writes_remain_visible() {
  TemporaryDirectory directory;
  auto db = open_db(directory.path, 1U << 20U);

  constexpr std::size_t thread_count = 8;
  constexpr std::size_t writes_per_thread = 100;
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      for (std::size_t index = 0; index < writes_per_thread; ++index) {
        const auto key =
            std::to_string(thread_id) + "-" + std::to_string(index);
        db->Put(key, "value");
      }
    });
  }
  for (auto& thread : threads) thread.join();

  for (std::size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    for (std::size_t index = 0; index < writes_per_thread; ++index) {
      const auto key =
          std::to_string(thread_id) + "-" + std::to_string(index);
      assert(text(db->Get(key)) == "value");
    }
  }
}

}  // namespace

int main() {
  test_memtable_overwrite_tombstone_and_order();
  test_newest_memtable_and_tombstone_win();
  test_capacity_freezes_memtable();
  test_concurrent_force_freeze_never_publishes_empty_memtable();
  test_concurrent_writes_remain_visible();
  std::cout << "Memtable tests passed\n";
}
