#include "hermesdb/hermesdb.hpp"
#include "hermesdb/key.hpp"
#include "hermesdb/memtable.hpp"

#include <atomic>
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
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic<unsigned long> sequence{};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-mvcc-x-" + std::string(label) + "-" +
            std::to_string(nonce) + "-" + std::to_string(sequence++));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

template <class Function>
void assert_throws(Function&& function) {
  bool threw = false;
  try {
    function();
  } catch (const Error&) {
    threw = true;
  }
  assert(threw);
}

Bytes bytes(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}

void internal_key_order_and_codec() {
  const InternalKey newest("shared", 9);
  const InternalKey older("shared", 2);
  assert(newest < older);
  assert(InternalKey("a", 1) < newest);
  assert(InternalKey::decode(newest.encode()) == newest);
  assert(newest.encode().size() == std::string_view("shared").size() + 8);
}

void memtable_keeps_versions() {
  MemTable table;
  table.put("k", 1, as_bytes("v1"));
  table.put("k", 3, as_bytes("v3"));
  table.put("k", 2, as_bytes("v2"));
  table.erase("gone", 4);
  assert(as_string(*table.get("k", 1)) == "v1");
  assert(as_string(*table.get("k", 2)) == "v2");
  assert(as_string(*table.get("k", 3)) == "v3");
  assert(as_string(*table.get("k", 100)) == "v3");
  assert(!table.get("k", 0));
  const auto tombstone = table.get("gone", 4);
  assert(tombstone.has_value());
  assert(tombstone->empty());
  assert(table.entries().size() == 4);
}

void write_batch_shares_one_timestamp() {
  TemporaryDirectory directory("batch-ts");
  auto db = DB::Open(directory.path);
  const std::vector<WriteBatchRecord> batch{
      PutRecord{bytes("a"), bytes("1")}, PutRecord{bytes("b"), bytes("2")}};
  db->WriteBatch(batch);
  assert(text(db->Get("a")) == "1");
  auto snapshot = db->NewTransaction();
  db->Put("a", "newer");
  assert(text(snapshot->Get("a")) == "1");
  assert(text(snapshot->Get("b")) == "2");
  snapshot.reset();
  db->Close();
}

void transaction_wal_survives_reopen() {
  TemporaryDirectory directory("txn-wal");
  Options options;
  options.enable_wal = true;
  options.compaction_options = NoCompactionOptions{};
  {
    auto db = DB::Open(directory.path, options);
    db->Put("keep", "old");
    auto txn = db->NewTransaction();
    txn->Put("keep", "committed");
    txn->Put("extra", "yes");
    txn->Commit();
    db->Sync();
    db->Close();
  }
  {
    auto db = DB::Open(directory.path, options);
    assert(text(db->Get("keep")) == "committed");
    assert(text(db->Get("extra")) == "yes");
    db->Close();
  }
}

void compaction_filter_waits_for_snapshots() {
  TemporaryDirectory directory("filter");
  auto db = DB::Open(directory.path);
  db->Put("drop/key", "old");
  db->Put("keep/key", "kept");
  auto reader = db->NewTransaction();
  db->Put("drop/key", "new");
  db->AddCompactionFilter(bytes("drop/"));
  db->ForceFullCompaction();
  assert(text(db->Get("drop/key")) == "new");
  assert(text(db->Get("keep/key")) == "kept");
  reader.reset();
  db->ForceFullCompaction();
  assert(!db->Get("drop/key"));
  assert(text(db->Get("keep/key")) == "kept");
  db->Close();
}

void conflict_after_flush() {
  TemporaryDirectory directory("conflict-flush");
  Options options;
  options.enable_wal = true;
  auto db = DB::Open(directory.path, options);
  db->Put("a", "base");
  auto loser = db->NewTransaction();
  auto winner = db->NewTransaction();
  winner->Put("a", "winner");
  winner->Commit();
  db->ForceFlush();
  loser->Put("a", "loser");
  assert_throws([&] { loser->Commit(); });
  assert(text(db->Get("a")) == "winner");
  db->Close();
}

void serializable_phantom_insert() {
  TemporaryDirectory directory("phantom");
  Options options;
  options.serializable = true;
  auto db = DB::Open(directory.path, options);
  auto missing_reader = db->NewTransaction();
  assert(!missing_reader->Get("missing"));
  db->Put("missing", "inserted");
  missing_reader->Put("dependent", "value");
  assert_throws([&] { missing_reader->Commit(); });
  assert(!db->Get("dependent"));
  db->Close();
}

void rejects_empty_keys_and_values() {
  TemporaryDirectory directory("validate");
  auto db = DB::Open(directory.path);
  assert_throws([&] { db->Put("", "value"); });
  assert_throws([&] { db->Put("key", ""); });
  assert_throws([&] { db->AddCompactionFilter(Bytes{}); });
  db->Close();
}

void snapshot_sees_flushed_old_version() {
  TemporaryDirectory directory("flush-snap");
  auto db = DB::Open(directory.path);
  db->Put("k", "mem");
  auto snapshot = db->NewTransaction();
  db->Put("k", "later");
  db->ForceFlush();
  db->ForceFullCompaction();
  assert(text(snapshot->Get("k")) == "mem");
  assert(text(db->Get("k")) == "later");
  snapshot.reset();
  db->ForceFullCompaction();
  assert(db->DumpStructure().find("versions=1") != std::string::npos);
  db->Close();
}

void concurrent_readers_during_compact() {
  TemporaryDirectory directory("concurrent");
  auto db = DB::Open(directory.path);
  db->Put("k", "v1");
  auto txn = db->NewTransaction();
  std::atomic<bool> stop{};
  std::atomic<int> seen{};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const auto value = txn->Get("k");
      assert(value);
      assert(as_string(*value) == "v1");
      seen.fetch_add(1, std::memory_order_relaxed);
    }
  });
  db->Put("k", "v2");
  db->ForceFullCompaction();
  stop.store(true, std::memory_order_relaxed);
  reader.join();
  assert(seen.load() > 0);
  assert(text(db->Get("k")) == "v2");
  txn.reset();
  db->Close();
}

}  // namespace

int main() {
  internal_key_order_and_codec();
  memtable_keeps_versions();
  write_batch_shares_one_timestamp();
  transaction_wal_survives_reopen();
  compaction_filter_waits_for_snapshots();
  conflict_after_flush();
  serializable_phantom_insert();
  rejects_empty_keys_and_values();
  snapshot_sees_flushed_old_version();
  concurrent_readers_during_compact();
  std::cout << "MVCC extended tests passed\n";
}
