#pragma once

#include "tiny_lsm/common.hpp"
#include "tiny_lsm/compaction.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tiny_lsm {

class Transaction;

enum class BoundKind { unbounded, included, excluded };

struct KeyBound {
  BoundKind kind{BoundKind::unbounded};
  Bytes key;

  static KeyBound Unbounded() { return {}; }
  static KeyBound Included(std::string_view key);
  static KeyBound Excluded(std::string_view key);
};

struct PutRecord {
  Bytes key;
  Bytes value;
};

struct DeleteRecord {
  Bytes key;
};

using WriteBatchRecord = std::variant<PutRecord, DeleteRecord>;

struct LsmStorageOptions {
  std::size_t block_size{4096};
  std::size_t target_sst_size{2U << 20U};
  std::size_t num_memtable_limit{3};
  CompactionOptions compaction_options{LeveledCompactionOptions{}};
  bool enable_wal{false};
  bool serializable{false};
};

class DbIterator {
 public:
  DbIterator();
  explicit DbIterator(std::vector<std::pair<Bytes, Bytes>> entries);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] ByteView key() const;
  [[nodiscard]] ByteView value() const;
  void next();

 private:
  std::vector<std::pair<Bytes, Bytes>> entries_;
  std::size_t index_{};
};

class MiniLsm : public std::enable_shared_from_this<MiniLsm> {
 public:
  static std::shared_ptr<MiniLsm> Open(
      const std::filesystem::path& path,
      LsmStorageOptions options = LsmStorageOptions{});

  ~MiniLsm();
  MiniLsm(const MiniLsm&) = delete;
  MiniLsm& operator=(const MiniLsm&) = delete;

  // TODO(week1-day1): point reads and mutable-memtable writes.
  [[nodiscard]] std::optional<Bytes> Get(ByteView key) const;
  [[nodiscard]] std::optional<Bytes> Get(std::string_view key) const;
  void Put(ByteView key, ByteView value);
  void Put(std::string_view key, std::string_view value);
  void Delete(ByteView key);
  void Delete(std::string_view key);
  // TODO(week2-day7): atomic write batches.
  void WriteBatch(std::span<const WriteBatchRecord> batch);
  // TODO(week1-day2): range iteration.
  [[nodiscard]] DbIterator Scan(
      KeyBound lower = KeyBound::Unbounded(),
      KeyBound upper = KeyBound::Unbounded()) const;

  // TODO(week2-day6): durability and recovery.
  void Sync();
  void Close();
  // TODO(week1-day1): mutable-to-immutable state transition.
  void ForceFreezeMemTable();
  // TODO(week1-day6): SST flushing.
  void ForceFlush();
  // TODO(week2-day1): compaction execution.
  void ForceFullCompaction();
  // TODO(week3-day7): compaction filters.
  void AddCompactionFilter(Bytes prefix);
  // TODO(week3-day3): MVCC transactions.
  [[nodiscard]] std::shared_ptr<Transaction> NewTransaction();
  // TODO(week1-day1): course state diagnostics.
  [[nodiscard]] std::string DumpStructure() const;

  // Idiomatic aliases keep the API comfortable for users coming from Rust.
  [[nodiscard]] std::optional<Bytes> get(std::string_view key) const {
    return Get(key);
  }
  void put(std::string_view key, std::string_view value) { Put(key, value); }
  void remove(std::string_view key) { Delete(key); }

 private:
  struct Impl;
  explicit MiniLsm(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class Transaction;
};

}  // namespace tiny_lsm
