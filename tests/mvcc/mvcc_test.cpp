#include "hermesdb/db.hpp"
#include "hermesdb/transaction.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string text(hermesdb::ByteView value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

struct TemporaryDirectory {
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic<unsigned long> sequence{};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("hermesdb-mvcc-" + std::string(label) + "-" +
            std::to_string(nonce) + "-" + std::to_string(sequence++));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

std::vector<std::pair<std::string, std::string>> collect(
    hermesdb::DbIterator iterator) {
  std::vector<std::pair<std::string, std::string>> entries;
  while (iterator.valid()) {
    entries.emplace_back(text(iterator.key()), text(iterator.value()));
    iterator.next();
  }
  return entries;
}

template <class Function>
void assert_throws(Function&& function) {
  bool threw = false;
  try {
    function();
  } catch (const hermesdb::Error&) {
    threw = true;
  }
  assert(threw);
}

void snapshot_isolation_and_read_your_writes() {
  TemporaryDirectory directory("snapshot");
  auto db = hermesdb::DB::Open(directory.path);
  db->Put("a", "a0");
  db->Put("b", "b0");
  db->Put("gone", "present");

  auto transaction = db->NewTransaction();
  const auto timestamp = transaction->read_timestamp();
  db->Put("a", "a1");
  db->Delete("b");
  db->Put("later", "outside");

  assert(text(*transaction->Get("a")) == "a0");
  assert(text(*transaction->Get("b")) == "b0");
  assert(!transaction->Get("later"));
  assert(transaction->read_timestamp() == timestamp);

  transaction->Put("new", "value");
  transaction->Delete("gone");
  assert(text(*transaction->Get("a")) == "a0");
  assert(text(*transaction->Get("new")) == "value");
  assert(!transaction->Get("gone"));

  const auto entries = collect(transaction->Scan());
  assert((entries == std::vector<std::pair<std::string, std::string>>{
                         {"a", "a0"}, {"b", "b0"}, {"new", "value"}}));
  transaction->Commit();
  assert(transaction->committed());
  assert(text(*db->Get("a")) == "a1");
  assert(text(*db->Get("new")) == "value");
  assert(!db->Get("gone"));
  assert_throws([&] { (void)transaction->Get("a"); });

  transaction.reset();
  db->Close();
}

void atomic_transaction_commit() {
  TemporaryDirectory directory("atomic");
  auto db = hermesdb::DB::Open(directory.path);
  db->Put("left", "old");
  db->Put("right", "old");

  auto transaction = db->NewTransaction();
  transaction->Put("left", "new");
  transaction->Put("right", "new");
  assert(text(*db->Get("left")) == "old");
  assert(text(*db->Get("right")) == "old");

  std::atomic<bool> stop{};
  std::thread observer([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const auto entries = collect(
          db->Scan(hermesdb::KeyBound::Included("left"),
                   hermesdb::KeyBound::Included("right")));
      assert(entries.size() == 2);
      assert(entries[0].second == entries[1].second);
    }
  });
  transaction->Commit();
  stop.store(true, std::memory_order_relaxed);
  observer.join();

  const auto entries = collect(db->Scan());
  assert((entries == std::vector<std::pair<std::string, std::string>>{
                         {"left", "new"}, {"right", "new"}}));
  transaction.reset();
  db->Close();
}

void scan_bounds_and_transaction_overlay() {
  TemporaryDirectory directory("bounds");
  auto db = hermesdb::DB::Open(directory.path);
  for (const auto* key : {"a", "b", "c", "d"}) db->Put(key, key);

  using hermesdb::KeyBound;
  assert((collect(db->Scan(KeyBound::Included("b"), KeyBound::Included("c"))) ==
          std::vector<std::pair<std::string, std::string>>{{"b", "b"},
                                                           {"c", "c"}}));
  assert((collect(db->Scan(KeyBound::Excluded("b"), KeyBound::Excluded("d"))) ==
          std::vector<std::pair<std::string, std::string>>{{"c", "c"}}));
  assert((collect(db->Scan(KeyBound::Unbounded(), KeyBound::Excluded("b"))) ==
          std::vector<std::pair<std::string, std::string>>{{"a", "a"}}));
  assert((collect(db->Scan(KeyBound::Excluded("c"), KeyBound::Unbounded())) ==
          std::vector<std::pair<std::string, std::string>>{{"d", "d"}}));
  assert(!db->Scan(KeyBound::Excluded("c"), KeyBound::Included("c")).valid());

  auto transaction = db->NewTransaction();
  transaction->Delete("b");
  transaction->Put("bb", "local");
  transaction->Put("d", "changed");
  assert((collect(transaction->Scan(KeyBound::Included("b"),
                                    KeyBound::Excluded("d"))) ==
          std::vector<std::pair<std::string, std::string>>{{"bb", "local"},
                                                           {"c", "c"}}));
  transaction.reset();
  db->Close();
}

void watermark_and_compaction_are_snapshot_safe() {
  TemporaryDirectory directory("watermark");
  auto db = hermesdb::DB::Open(directory.path);
  db->Put("key", "v1");
  auto old_snapshot = db->NewTransaction();
  db->Put("key", "v2");
  db->Put("key", "v3");

  assert(db->DumpStructure().find("versions=3") != std::string::npos);
  assert(db->DumpStructure().find("active_snapshots=1") != std::string::npos);
  db->ForceFullCompaction();
  assert(text(*old_snapshot->Get("key")) == "v1");
  assert(text(*db->Get("key")) == "v3");
  assert(db->DumpStructure().find("versions=3") != std::string::npos);

  old_snapshot.reset();
  db->ForceFullCompaction();
  assert(text(*db->Get("key")) == "v3");
  assert(db->DumpStructure().find("versions=1") != std::string::npos);
  assert(db->DumpStructure().find("active_snapshots=0") != std::string::npos);

  db->Put("deleted", "old");
  auto deletion_snapshot = db->NewTransaction();
  db->Delete("deleted");
  db->ForceFullCompaction();
  assert(text(*deletion_snapshot->Get("deleted")) == "old");
  assert(!db->Get("deleted"));
  deletion_snapshot.reset();
  db->ForceFullCompaction();
  assert(db->DumpStructure().find("keys=1") != std::string::npos);
  db->Close();
}

void serializable_write_skew_is_rejected() {
  TemporaryDirectory directory("write-skew");
  hermesdb::Options options;
  options.serializable = true;
  auto db = hermesdb::DB::Open(directory.path, options);
  db->Put("doctor-a", "on");
  db->Put("doctor-b", "on");

  auto first = db->NewTransaction();
  auto second = db->NewTransaction();
  assert(text(*first->Get("doctor-a")) == "on");
  assert(text(*first->Get("doctor-b")) == "on");
  assert(text(*second->Get("doctor-a")) == "on");
  assert(text(*second->Get("doctor-b")) == "on");

  first->Put("doctor-a", "off");
  second->Put("doctor-b", "off");
  first->Commit();
  assert_throws([&] { second->Commit(); });
  assert(text(*db->Get("doctor-a")) == "off");
  assert(text(*db->Get("doctor-b")) == "on");

  first.reset();
  second.reset();
  db->Close();
}

void write_write_conflict_is_rejected() {
  TemporaryDirectory directory("ww");
  auto db = hermesdb::DB::Open(directory.path);
  db->Put("a", "base");
  auto loser = db->NewTransaction();
  auto winner = db->NewTransaction();
  winner->Put("a", "winner");
  winner->Commit();
  loser->Put("a", "loser");
  assert_throws([&] { loser->Commit(); });
  assert(text(*db->Get("a")) == "winner");
  loser.reset();
  winner.reset();
  db->Close();
}

void wal_recovery_and_serializable_put() {
  TemporaryDirectory directory("engine");
  hermesdb::Options options;
  options.enable_wal = true;
  {
    auto db = hermesdb::DB::Open(directory.path, options);
    db->Put("a", "one");
    db->Put("b", "two");
    db->Delete("a");
    db->Sync();
    assert(!db->Get("a"));
    assert(text(*db->Get("b")) == "two");
    db->Close();
  }
  {
    auto db = hermesdb::DB::Open(directory.path, options);
    assert(!db->Get("a"));
    assert(text(*db->Get("b")) == "two");
    db->ForceFlush();
    db->Close();
  }
}

}  // namespace

int main() {
  snapshot_isolation_and_read_your_writes();
  atomic_transaction_commit();
  scan_bounds_and_transaction_overlay();
  watermark_and_compaction_are_snapshot_safe();
  serializable_write_skew_is_rejected();
  write_write_conflict_is_rejected();
  wal_recovery_and_serializable_put();
  std::cout << "MVCC tests passed\n";
}
