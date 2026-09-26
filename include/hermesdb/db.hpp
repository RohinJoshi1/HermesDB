#pragma once

#include "hermesdb/common.hpp"
#include "hermesdb/compaction.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hermesdb {

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

struct Options {
  std::size_t block_size{4096};
  std::size_t target_sst_size{2U << 20U};
  std::size_t num_memtable_limit{3};
  CompactionOptions compaction_options{LeveledCompactionOptions{}};
  Compression compression{Compression::none};
  std::size_t block_cache_capacity{4096};
  bool enable_wal{false};
  bool serializable{false};
};

// Counters used by the Phase 1 benchmark suite. Live layout fields are a
// point-in-time snapshot; the rest are monotonic since Open.
struct DbMetrics {
  std::uint64_t write_ops{};
  std::uint64_t write_user_bytes{};
  std::uint64_t wal_bytes{};
  std::uint64_t get_ops{};
  std::uint64_t get_hits{};
  std::uint64_t scan_ops{};
  std::uint64_t scan_keys{};
  std::uint64_t freeze_count{};
  std::uint64_t flush_count{};
  std::uint64_t flush_bytes{};
  std::uint64_t compaction_count{};
  std::uint64_t compaction_input_bytes{};
  std::uint64_t compaction_output_bytes{};
  std::uint64_t sst_raw_bytes{};
  std::uint64_t sst_stored_bytes{};
  std::uint64_t block_cache_hits{};
  std::uint64_t block_cache_misses{};
  std::size_t immutable_memtables{};
  std::size_t l0_tables{};
  std::size_t l1_tables{};
};

class DbIterator {
 public:
  DbIterator();
  DbIterator(DbIterator&&) noexcept;
  DbIterator& operator=(DbIterator&&) noexcept;
  ~DbIterator();

  DbIterator(const DbIterator&) = delete;
  DbIterator& operator=(const DbIterator&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  // Views are valid until the next next() or destruction.
  [[nodiscard]] ByteView key() const;
  [[nodiscard]] ByteView value() const;
  void next();

 private:
  struct Impl;
  friend class DB;
  friend class Transaction;
  explicit DbIterator(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class DB : public std::enable_shared_from_this<DB> {
 public:
  static std::shared_ptr<DB> Open(
      const std::filesystem::path& path,
      Options options = Options{});

  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  [[nodiscard]] std::optional<Bytes> Get(ByteView key) const;
  [[nodiscard]] std::optional<Bytes> Get(std::string_view key) const;
  void Put(ByteView key, ByteView value);
  void Put(std::string_view key, std::string_view value);
  void Delete(ByteView key);
  void Delete(std::string_view key);
  void WriteBatch(std::span<const WriteBatchRecord> batch);
  [[nodiscard]] DbIterator Scan(
      KeyBound lower = KeyBound::Unbounded(),
      KeyBound upper = KeyBound::Unbounded()) const;

  void Sync();
  void Close();
  void ForceFreezeMemTable();
  void ForceFlush();
  void ForceFullCompaction();
  void AddCompactionFilter(Bytes prefix);
  [[nodiscard]] std::shared_ptr<Transaction> NewTransaction();
  [[nodiscard]] std::string DumpStructure() const;
  [[nodiscard]] DbMetrics Metrics() const;

  // Idiomatic aliases keep the API comfortable for users coming from Rust.
  [[nodiscard]] std::optional<Bytes> get(std::string_view key) const {
    return Get(key);
  }
  void put(std::string_view key, std::string_view value) { Put(key, value); }
  void remove(std::string_view key) { Delete(key); }

 private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class Transaction;
  friend struct DbIterator::Impl;
};

}  // namespace hermesdb
