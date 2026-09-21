#include "hermesdb/db.hpp"

#include "hermesdb/memtable.hpp"
#include "hermesdb/persistence.hpp"
#include "hermesdb/table.hpp"
#include "hermesdb/transaction.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <span>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <variant>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hermesdb {
namespace {

Bytes owned(ByteView value) { return {value.begin(), value.end()}; }

Bytes owned(std::string_view value) {
  return owned(as_bytes(value));
}

void validate_key(ByteView key) {
  if (key.empty()) throw Error("keys must not be empty");
}

void validate_value(ByteView value) {
  if (value.empty()) {
    throw Error("empty values are reserved for deletion tombstones");
  }
}

// Global timestamps stay unique, but Put does not wait for a closed prefix.
// READY slots are packed (8 B). Snapshots wait until the prefix covers every
// timestamp claimed before begin. Plain Get/Scan read at `claimed()` so
// read-your-writes does not join the prefix chain.
class TimestampRings {
 public:
  static constexpr std::size_t kCapacity = 8192;
  static constexpr std::size_t kMask = kCapacity - 1;

  [[nodiscard]] std::uint64_t visible() const noexcept {
    return visible_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t claimed() const noexcept {
    return next_.load(std::memory_order_acquire);
  }

  void ensure_space() {
    while (next_.load(std::memory_order_relaxed) - visible() >= kCapacity) {
      close_prefix();
      std::this_thread::yield();
    }
  }

  [[nodiscard]] std::uint64_t claim() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void complete(std::uint64_t timestamp) noexcept {
    write_[timestamp & kMask].store(timestamp, std::memory_order_release);
    close_prefix();
  }

  void wait_quiesced() {
    wait_closed(claimed());
  }

  void observe(std::uint64_t timestamp) {
    auto bump = [](std::atomic<std::uint64_t>& cell, std::uint64_t value) {
      auto current = cell.load(std::memory_order_relaxed);
      while (value > current &&
             !cell.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
      }
    };
    bump(next_, timestamp);
    bump(visible_, timestamp);
  }

 private:
  void wait_closed(std::uint64_t target) {
    while (visible() < target) {
      close_prefix();
      std::this_thread::yield();
    }
  }

  void close_prefix() noexcept {
    auto v = visible_.load(std::memory_order_acquire);
    for (;;) {
      auto n = v;
      while (write_[(n + 1) & kMask].load(std::memory_order_acquire) == n + 1) {
        ++n;
      }
      if (n == v) return;
      if (visible_.compare_exchange_weak(v, n, std::memory_order_release,
                                         std::memory_order_acquire)) {
        v = n;
      }
    }
  }

  alignas(64) std::atomic<std::uint64_t> write_[kCapacity]{};
  alignas(64) std::atomic<std::uint64_t> visible_{0};
  alignas(64) std::atomic<std::uint64_t> next_{0};
};

void sync_path(const std::filesystem::path& path, bool directory = false) {
#ifndef _WIN32
  const int flags = directory ? O_RDONLY : O_RDWR;
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) throw Error("failed to open path for sync");
#ifdef F_FULLFSYNC
  int result = ::fcntl(fd, F_FULLFSYNC, 0);
  if (result != 0) result = ::fsync(fd);
#else
  const int result = ::fsync(fd);
#endif
  const int close_result = ::close(fd);
  if (result != 0 || close_result != 0) throw Error("failed to sync path");
#else
  static_cast<void>(path);
  static_cast<void>(directory);
#endif
}

void persist_sst(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 const Bytes& encoded) {
  const std::span<const std::byte> view{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  persistence::write_file(temporary, view);
  std::filesystem::rename(temporary, destination);
  sync_path(destination.parent_path(), true);
}

std::uint64_t mvcc_wal_frame_bytes(
    std::span<const persistence::MvccWalRecord> records) {
  std::uint64_t bytes = 8;  // payload-size + crc
  for (const auto& record : records) {
    bytes += 2 + record.key.size() + 8 + 2 + record.value.size();
  }
  return bytes;
}

persistence::Bytes persistent(ByteView bytes) {
  persistence::Bytes result;
  result.reserve(bytes.size());
  for (const auto byte : bytes) result.push_back(static_cast<std::byte>(byte));
  return result;
}

Bytes regular(std::span<const std::byte> bytes) {
  Bytes result;
  result.reserve(bytes.size());
  for (const auto byte : bytes)
    result.push_back(std::to_integer<std::uint8_t>(byte));
  return result;
}

void append_id(persistence::Bytes& bytes, std::uint64_t id) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<std::byte>(id >> shift));
}

std::vector<std::uint64_t> decode_ids(std::span<const std::byte> bytes) {
  if (bytes.size() % 8 != 0) throw Error("invalid compaction manifest task");
  std::vector<std::uint64_t> ids;
  for (std::size_t at = 0; at != bytes.size(); at += 8) {
    std::uint64_t id = 0;
    for (std::size_t index = 0; index != 8; ++index)
      id = (id << 8) | std::to_integer<std::uint8_t>(bytes[at + index]);
    ids.push_back(id);
  }
  return ids;
}

}  // namespace

KeyBound KeyBound::Included(std::string_view key) {
  return {BoundKind::included, owned(key)};
}

KeyBound KeyBound::Excluded(std::string_view key) {
  return {BoundKind::excluded, owned(key)};
}

DbIterator::DbIterator() = default;

DbIterator::DbIterator(std::vector<std::pair<Bytes, Bytes>> entries)
    : entries_(std::move(entries)) {}

bool DbIterator::valid() const noexcept { return index_ < entries_.size(); }

ByteView DbIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return entries_[index_].first;
}

ByteView DbIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return entries_[index_].second;
}

void DbIterator::next() {
  if (valid()) ++index_;
}

struct DB::Impl {
  struct MemTableSlot {
    std::uint64_t id{};
    std::shared_ptr<MemTable> table;
    std::shared_ptr<persistence::MvccWal> wal;
  };

  struct TableSlot {
    std::uint64_t id{};
    std::shared_ptr<const Table> table;
    InternalKey first_key;
    InternalKey last_key;
  };

  struct State {
    MemTableSlot mutable_memtable;
    std::vector<MemTableSlot> immutable_memtables;
    // Level-0 is newest first because ranges may overlap.
    std::vector<TableSlot> l0_tables;
    // Level-1 is a sorted, non-overlapping run.
    std::vector<TableSlot> l1_tables;
  };

  std::filesystem::path path;
  Options options;
  mutable std::shared_mutex state_mutex;
  std::mutex state_change_mutex;
  std::mutex flush_mutex;
  std::shared_ptr<const State> state;
  std::uint64_t next_memtable_id{1};
  TimestampRings timestamps;
  std::mutex commit_mutex;
  mutable std::mutex readers_mutex;
  std::map<std::uint64_t, std::size_t> readers;
  std::map<std::uint64_t, std::set<Bytes>> committed_writes;
  mutable std::mutex filters_mutex;
  std::vector<Bytes> compaction_filters;
  std::atomic<bool> closed{false};
  std::atomic<bool> stopping{false};
  std::mutex worker_mutex;
  std::condition_variable worker_wake;
  std::thread compaction_worker;
  std::thread flush_thread;
  std::unique_ptr<persistence::Manifest> manifest;
  std::vector<std::uint64_t> recovered_memtable_ids;
  mutable std::unique_ptr<BlockCache> block_cache;

  struct Counters {
    std::atomic<std::uint64_t> write_ops{0};
    std::atomic<std::uint64_t> write_user_bytes{0};
    std::atomic<std::uint64_t> wal_bytes{0};
    std::atomic<std::uint64_t> get_ops{0};
    std::atomic<std::uint64_t> get_hits{0};
    std::atomic<std::uint64_t> scan_ops{0};
    std::atomic<std::uint64_t> scan_keys{0};
    std::atomic<std::uint64_t> freeze_count{0};
    std::atomic<std::uint64_t> flush_count{0};
    std::atomic<std::uint64_t> flush_bytes{0};
    std::atomic<std::uint64_t> compaction_count{0};
    std::atomic<std::uint64_t> compaction_input_bytes{0};
    std::atomic<std::uint64_t> compaction_output_bytes{0};
    std::atomic<std::uint64_t> sst_raw_bytes{0};
    std::atomic<std::uint64_t> sst_stored_bytes{0};
  };
  mutable Counters counters;

  Impl(std::filesystem::path path_arg, Options options_arg)
      : path(std::move(path_arg)), options(std::move(options_arg)) {
    if (options.block_cache_capacity > 0) {
      block_cache = std::make_unique<BlockCache>(options.block_cache_capacity);
    }
    auto initial = std::make_shared<State>();
    initial->mutable_memtable = {0, std::make_shared<MemTable>(), nullptr};
    state = std::move(initial);
  }

  void require_open() const {
    if (closed.load(std::memory_order_acquire)) {
      throw Error("database is closed");
    }
  }

  std::shared_ptr<const State> state_snapshot() const {
    std::shared_lock lock(state_mutex);
    return state;
  }

  void add_reader(std::uint64_t timestamp) {
    std::lock_guard lock(readers_mutex);
    ++readers[timestamp];
  }

  void remove_reader(std::uint64_t timestamp) {
    std::lock_guard lock(readers_mutex);
    const auto found = readers.find(timestamp);
    if (found == readers.end()) return;
    if (--found->second == 0) readers.erase(found);
  }

  std::uint64_t watermark() const {
    std::lock_guard lock(readers_mutex);
    return readers.empty() ? timestamps.visible() : readers.begin()->first;
  }

  void observe_timestamp(std::uint64_t timestamp) {
    timestamps.observe(timestamp);
  }

  std::size_t active_readers() const {
    std::lock_guard lock(readers_mutex);
    std::size_t total = 0;
    for (const auto& [timestamp, count] : readers) {
      static_cast<void>(timestamp);
      total += count;
    }
    return total;
  }

  TableSlot open_table(std::uint64_t id) const {
    const auto file = path / (std::to_string(id) + ".sst");
    auto table = Table::open(file);
    return {id, table, table->block_meta().front().first_key,
            table->block_meta().back().last_key};
  }

  void recover_manifest() {
    const auto manifest_path = path / "MANIFEST";
    if (!std::filesystem::exists(manifest_path)) {
      manifest = persistence::Manifest::create(manifest_path);
      sync_path(path, true);
      return;
    }
    auto recovery = persistence::Manifest::recover(manifest_path);
    auto recovered = std::make_shared<State>(*state);
    std::vector<std::uint64_t> l0_ids;
    std::vector<std::uint64_t> l1_ids;
    std::unordered_set<std::uint64_t> live_memtables;
    for (const auto& raw : recovery.records) {
      const auto record = persistence::decode_manifest_payload(raw.payload);
      next_memtable_id = std::max(next_memtable_id, record.object_id + 1);
      for (const auto id : record.output_ids)
        next_memtable_id = std::max(next_memtable_id, id + 1);
      if (record.kind == persistence::ManifestRecordKind::new_memtable) {
        live_memtables.insert(record.object_id);
        continue;
      }
      if (record.kind == persistence::ManifestRecordKind::flush) {
        live_memtables.erase(record.object_id);
        l0_ids.insert(l0_ids.begin(), record.object_id);
        continue;
      }
      const auto captured = decode_ids(record.compaction_task);
      const std::unordered_set<std::uint64_t> captured_set(captured.begin(),
                                                           captured.end());
      std::erase_if(l0_ids, [&](std::uint64_t id) {
        return captured_set.contains(id);
      });
      std::erase_if(l1_ids, [&](std::uint64_t id) {
        return captured_set.contains(id);
      });
      l1_ids.insert(l1_ids.end(), record.output_ids.begin(),
                    record.output_ids.end());
    }
    for (const auto id : l0_ids) recovered->l0_tables.push_back(open_table(id));
    for (const auto id : l1_ids) recovered->l1_tables.push_back(open_table(id));
    std::sort(recovered->l1_tables.begin(), recovered->l1_tables.end(),
              [](const TableSlot& lhs, const TableSlot& rhs) {
                return lhs.first_key < rhs.first_key;
              });
    state = std::move(recovered);
    for (const auto& slot : state->l0_tables)
      for (auto it = slot.table->iter(); it->valid(); it->next())
        observe_timestamp(it->key().timestamp());
    for (const auto& slot : state->l1_tables)
      for (auto it = slot.table->iter(); it->valid(); it->next())
        observe_timestamp(it->key().timestamp());
    manifest = std::move(recovery.manifest);
    recovered_memtable_ids.assign(live_memtables.begin(), live_memtables.end());
    std::sort(recovered_memtable_ids.begin(), recovered_memtable_ids.end());
    if (!options.enable_wal) {
      const auto fresh_id = next_memtable_id++;
      auto next = std::make_shared<State>(*state);
      next->mutable_memtable = {fresh_id, std::make_shared<MemTable>(), nullptr};
      state = std::move(next);
      manifest->append(persistence::InternalManifestRecord{
          persistence::ManifestRecordKind::new_memtable, fresh_id, {}, {}});
    }
  }

  void recover_wals() {
    if (!options.enable_wal) return;
    if (recovered_memtable_ids.empty()) {
      const auto id = state->mutable_memtable.id;
      const auto wal_path = path / (std::to_string(id) + ".wal");
      auto wal = std::shared_ptr<persistence::MvccWal>(
          persistence::MvccWal::create(wal_path).release());
      sync_path(path, true);
      manifest->append(persistence::InternalManifestRecord{
          persistence::ManifestRecordKind::new_memtable, id, {}, {}});
      auto next = std::make_shared<State>(*state);
      next->mutable_memtable = {id, std::make_shared<MemTable>(), wal};
      state = std::move(next);
      next_memtable_id = std::max(next_memtable_id, id + 1);
      return;
    }

    std::vector<MemTableSlot> slots;
    for (const auto id : recovered_memtable_ids) {
      const auto wal_path = path / (std::to_string(id) + ".wal");
      if (!std::filesystem::exists(wal_path))
        throw Error("manifest references a missing WAL");
      auto recovery = persistence::MvccWal::recover(wal_path);
      auto table = std::make_shared<MemTable>();
      for (const auto& record : recovery.records()) {
        const auto key = regular(record.key);
        const auto value = regular(record.value);
        table->put(key, record.timestamp, value);
        observe_timestamp(record.timestamp);
      }
      slots.push_back(
          {id, table,
           std::shared_ptr<persistence::MvccWal>(recovery.wal.release())});
    }
    auto next = std::make_shared<State>(*state);
    next->mutable_memtable = slots.back();
    for (auto iterator = slots.rbegin() + 1; iterator != slots.rend();
         ++iterator) {
      next->immutable_memtables.push_back(*iterator);
    }
    state = std::move(next);
  }

  void maybe_freeze(const std::shared_ptr<MemTable>& expected) {
    if (expected->approximate_size() < options.target_sst_size) return;

    std::shared_ptr<persistence::MvccWal> old_wal;
    {
      std::unique_lock change_lock(state_change_mutex, std::try_to_lock);
      if (!change_lock.owns_lock()) return;
      std::unique_lock state_lock(state_mutex);
      if (state->mutable_memtable.table != expected ||
          expected->approximate_size() < options.target_sst_size) {
        return;
      }
      old_wal = freeze_locked();
    }
    if (old_wal) old_wal->flush();
  }

  void force_freeze() {
    require_open();
    const auto snapshot = state_snapshot();
    const auto expected = snapshot->mutable_memtable.table;

    std::shared_ptr<persistence::MvccWal> old_wal;
    {
      std::unique_lock change_lock(state_change_mutex);
      std::unique_lock state_lock(state_mutex);
      if (state->mutable_memtable.table != expected || expected->empty()) {
        return;
      }
      old_wal = freeze_locked();
    }
    if (old_wal) old_wal->flush();
  }

  bool flush_oldest_immutable() {
    std::unique_lock flush_lock(flush_mutex);
    MemTableSlot source;
    {
      std::unique_lock change_lock(state_change_mutex);
      std::shared_lock state_lock(state_mutex);
      if (state->immutable_memtables.empty()) return false;
      source = state->immutable_memtables.back();
    }

    const auto entries = source.table->entries();
    if (entries.empty()) throw Error("cannot flush an empty memtable");
    TableBuilder builder(options.block_size, options.compression);
    for (const auto& [key, value] : entries) {
      builder.add(key, value);
    }
    Bytes encoded = builder.finish();
    const auto temporary = path / (std::to_string(source.id) + ".sst.tmp");
    const auto destination = path / (std::to_string(source.id) + ".sst");
    persist_sst(temporary, destination, encoded);
    const auto flushed = encoded.size();
    auto table = Table::open(destination);

    {
      std::unique_lock change_lock(state_change_mutex);
      std::unique_lock state_lock(state_mutex);
      if (state->immutable_memtables.empty() ||
          state->immutable_memtables.back().id != source.id) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        return false;
      }
      counters.sst_raw_bytes.fetch_add(builder.raw_block_bytes(),
                                       std::memory_order_relaxed);
      counters.sst_stored_bytes.fetch_add(flushed, std::memory_order_relaxed);
      counters.flush_count.fetch_add(1, std::memory_order_relaxed);
      counters.flush_bytes.fetch_add(flushed, std::memory_order_relaxed);
      manifest->append(persistence::InternalManifestRecord{
          persistence::ManifestRecordKind::flush, source.id, {}, {}});
      auto next = std::make_shared<State>(*state);
      next->immutable_memtables.pop_back();
      next->l0_tables.insert(
          next->l0_tables.begin(),
          TableSlot{source.id, table, table->block_meta().front().first_key,
                    table->block_meta().back().last_key});
      state = std::move(next);
    }
    if (source.wal) {
      source.wal->flush();
      source.wal.reset();
      std::error_code ignored;
      std::filesystem::remove(
          path / (std::to_string(source.id) + ".wal"), ignored);
      sync_path(path, true);
    }
    worker_wake.notify_one();
    return true;
  }

  std::size_t compaction_trigger() const {
    return std::visit(
        [](const auto& policy) -> std::size_t {
          using Policy = std::decay_t<decltype(policy)>;
          if constexpr (std::is_same_v<Policy, LeveledCompactionOptions> ||
                        std::is_same_v<Policy,
                                       SimpleLeveledCompactionOptions>) {
            return policy.level0_file_num_compaction_trigger;
          } else if constexpr (std::is_same_v<Policy,
                                              TieredCompactionOptions>) {
            return policy.num_tiers;
          } else {
            return 0;
          }
        },
        options.compaction_options);
  }

  void run_compaction_worker() {
    std::unique_lock worker_lock(worker_mutex);
    while (!stopping.load(std::memory_order_acquire)) {
      worker_wake.wait_for(worker_lock, std::chrono::milliseconds(20));
      if (stopping.load(std::memory_order_acquire)) break;
      const auto trigger = compaction_trigger();
      if (trigger == 0 || state_snapshot()->l0_tables.size() < trigger) continue;
      worker_lock.unlock();
      try {
        force_full_compaction();
      } catch (...) {
        // A foreground operation will expose persistent I/O failures. A stale
        // task is harmless because result installation removes captured IDs.
      }
      worker_lock.lock();
    }
  }

  TableSlot write_table(std::uint64_t id,
                        const std::vector<KeyValue>& entries) {
    if (entries.empty()) throw Error("cannot write an empty SST");
    TableBuilder builder(options.block_size, options.compression);
    for (const auto& [key, value] : entries) builder.add(key, value);
    Bytes encoded = builder.finish();
    const auto temporary = path / (std::to_string(id) + ".sst.tmp");
    const auto destination = path / (std::to_string(id) + ".sst");
    persist_sst(temporary, destination, encoded);
    counters.sst_raw_bytes.fetch_add(builder.raw_block_bytes(),
                                     std::memory_order_relaxed);
    counters.sst_stored_bytes.fetch_add(encoded.size(),
                                        std::memory_order_relaxed);
    auto table = Table::open(destination);
    return {id, table, table->block_meta().front().first_key,
            table->block_meta().back().last_key};
  }

  void force_full_compaction() {
    require_open();
    std::vector<TableSlot> captured_l0;
    std::vector<TableSlot> captured_l1;
    std::uint64_t output_id{};
    {
      std::unique_lock state_lock(state_mutex);
      captured_l0 = state->l0_tables;
      captured_l1 = state->l1_tables;
      if (captured_l0.empty() && captured_l1.empty()) return;
      output_id = next_memtable_id++;
    }

    // Merge without either structural mutex. Sources are immutable and held
    // alive by this captured snapshot. Newest L0 tables have first priority.
    std::map<InternalKey, Bytes, InternalKeyLess> merged;
    const auto add = [&merged](const TableSlot& slot) {
      for (auto iterator = slot.table->iter(); iterator->valid();
           iterator->next()) {
        merged.try_emplace(
            iterator->key(),
            Bytes(iterator->value().begin(), iterator->value().end()));
      }
    };
    for (const auto& table : captured_l0) add(table);
    for (const auto& table : captured_l1) add(table);

    const auto gc_watermark = watermark();
    std::vector<Bytes> filters;
    {
      std::lock_guard lock(filters_mutex);
      filters = compaction_filters;
    }
    std::vector<KeyValue> live;
    Bytes previous_user_key;
    bool kept_at_or_below = false;
    bool filtered = false;
    for (auto& [key, value] : merged) {
      const Bytes user_key(key.user_key().begin(), key.user_key().end());
      if (user_key != previous_user_key) {
        previous_user_key = user_key;
        kept_at_or_below = false;
        filtered = std::any_of(
            filters.begin(), filters.end(), [&](const Bytes& prefix) {
              return prefix.size() <= user_key.size() &&
                     std::equal(prefix.begin(), prefix.end(),
                                user_key.begin());
            });
      }
      if (key.timestamp() > gc_watermark) {
        live.emplace_back(std::move(key), std::move(value));
      } else if (filtered) {
        kept_at_or_below = true;
      } else if (!kept_at_or_below) {
        kept_at_or_below = true;
        // Full compaction reaches the bottom level: a visible tombstone and
        // every older version can disappear together.
        if (!value.empty())
          live.emplace_back(std::move(key), std::move(value));
      }
    }
    std::optional<TableSlot> output;
    if (!live.empty()) output = write_table(output_id, live);
    std::uint64_t input_bytes = 0;
    for (const auto& table : captured_l0)
      input_bytes += table.table->file_size();
    for (const auto& table : captured_l1)
      input_bytes += table.table->file_size();
    counters.compaction_count.fetch_add(1, std::memory_order_relaxed);
    counters.compaction_input_bytes.fetch_add(input_bytes,
                                              std::memory_order_relaxed);
    if (output) {
      counters.compaction_output_bytes.fetch_add(
          output->table->file_size(), std::memory_order_relaxed);
    }

    std::unordered_set<std::uint64_t> captured_ids;
    for (const auto& table : captured_l0) captured_ids.insert(table.id);
    for (const auto& table : captured_l1) captured_ids.insert(table.id);
    persistence::Bytes task_bytes;
    for (const auto id : captured_ids) append_id(task_bytes, id);
    manifest->append(persistence::InternalManifestRecord{
        persistence::ManifestRecordKind::compaction, 0,
        std::move(task_bytes), output ? std::vector<std::uint64_t>{output_id}
                                     : std::vector<std::uint64_t>{}});
    {
      std::unique_lock change_lock(state_change_mutex);
      std::unique_lock state_lock(state_mutex);
      auto next = std::make_shared<State>(*state);
      std::erase_if(next->l0_tables, [&](const TableSlot& table) {
        return captured_ids.contains(table.id);
      });
      std::erase_if(next->l1_tables, [&](const TableSlot& table) {
        return captured_ids.contains(table.id);
      });
      if (output) next->l1_tables.push_back(*output);
      std::sort(next->l1_tables.begin(), next->l1_tables.end(),
                [](const TableSlot& lhs, const TableSlot& rhs) {
                  return lhs.first_key < rhs.first_key;
                });
      state = std::move(next);
    }
    for (const auto id : captured_ids) {
      std::error_code ignored;
      std::filesystem::remove(path / (std::to_string(id) + ".sst"), ignored);
    }
    sync_path(path, true);
  }

  void trigger_flush() {
    if (closed.load(std::memory_order_acquire)) return;
    const auto snapshot = state_snapshot();
    if (snapshot->immutable_memtables.size() < options.num_memtable_limit) {
      return;
    }
    static_cast<void>(flush_oldest_immutable());
  }

  void flush_loop() {
    std::unique_lock lock(worker_mutex);
    while (!stopping.load(std::memory_order_acquire)) {
      worker_wake.wait_for(lock, std::chrono::milliseconds(50));
      if (stopping.load(std::memory_order_acquire)) break;
      lock.unlock();
      try {
        trigger_flush();
      } catch (...) {
      }
      lock.lock();
    }
  }

  void start_workers() {
    flush_thread = std::thread([this] { flush_loop(); });
    compaction_worker = std::thread([this] { run_compaction_worker(); });
  }

  void stop_worker() {
    stopping.store(true, std::memory_order_release);
    worker_wake.notify_all();
    if (flush_thread.joinable() &&
        flush_thread.get_id() != std::this_thread::get_id()) {
      flush_thread.join();
    }
    if (compaction_worker.joinable() &&
        compaction_worker.get_id() != std::this_thread::get_id()) {
      compaction_worker.join();
    }
  }

  void force_flush() {
    require_open();
    force_freeze();
    while (flush_oldest_immutable()) {
    }
  }

  std::shared_ptr<persistence::MvccWal> freeze_locked() {
    auto next = std::make_shared<State>(*state);
    next->immutable_memtables.insert(next->immutable_memtables.begin(),
                                     next->mutable_memtable);
    auto old_wal = next->mutable_memtable.wal;
    const auto new_id = next_memtable_id++;
    std::shared_ptr<persistence::MvccWal> wal;
    if (options.enable_wal) {
      wal = std::shared_ptr<persistence::MvccWal>(
          persistence::MvccWal::create(
              path / (std::to_string(new_id) + ".wal")).release());
      sync_path(path, true);
    }
    manifest->append(persistence::InternalManifestRecord{
        persistence::ManifestRecordKind::new_memtable, new_id, {}, {}});
    next->mutable_memtable = {new_id, std::make_shared<MemTable>(), wal};
    state = std::move(next);
    counters.freeze_count.fetch_add(1, std::memory_order_relaxed);
    worker_wake.notify_all();
    return old_wal;
  }

  std::uint64_t write_entries_locked(
      std::span<const std::pair<Bytes, Bytes>> entries,
      std::shared_ptr<persistence::MvccWal>* ready_wal = nullptr) {
    require_open();
    if (entries.empty()) {
      return timestamps.visible();
    }
    timestamps.ensure_space();
    const auto timestamp = timestamps.claim();
    std::vector<KeyValue> versioned;
    std::vector<persistence::MvccWalRecord> wal_records;
    versioned.reserve(entries.size());
    wal_records.reserve(entries.size());
    for (const auto& [key, value] : entries) {
      versioned.emplace_back(InternalKey(key, timestamp), value);
      wal_records.push_back({persistent(key), timestamp, persistent(value)});
    }
    std::shared_ptr<MemTable> current;
    std::shared_ptr<persistence::MvccWal> wal;
    {
      struct Complete {
        TimestampRings* rings;
        std::uint64_t ts;
        ~Complete() { rings->complete(ts); }
      } complete{&timestamps, timestamp};
      std::shared_lock lock(state_mutex);
      current = state->mutable_memtable.table;
      wal = state->mutable_memtable.wal;
      if (wal) {
        wal->append_batch(wal_records);
        counters.wal_bytes.fetch_add(mvcc_wal_frame_bytes(wal_records),
                                     std::memory_order_relaxed);
      }
      current->put_batch(versioned);
    }
    std::uint64_t user_bytes = 0;
    for (const auto& [key, value] : entries)
      user_bytes += key.size() + value.size();
    counters.write_ops.fetch_add(entries.size(), std::memory_order_relaxed);
    counters.write_user_bytes.fetch_add(user_bytes, std::memory_order_relaxed);
    maybe_freeze(current);
    if (ready_wal) *ready_wal = std::move(wal);
    return timestamp;
  }

  std::uint64_t write_entries(
      std::span<const std::pair<Bytes, Bytes>> entries) {
    std::shared_ptr<persistence::MvccWal> wal;
    const auto timestamp = write_entries_locked(entries, &wal);
    if (wal) wal->flush_if_needed();
    return timestamp;
  }

  void put(ByteView key, ByteView value) {
    validate_key(key);
    validate_value(value);
    const std::array<std::pair<Bytes, Bytes>, 1> entry{
        std::pair{owned(key), owned(value)}};
    static_cast<void>(write_entries(entry));
  }

  void erase(ByteView key) {
    validate_key(key);
    const std::array<std::pair<Bytes, Bytes>, 1> entry{
        std::pair{owned(key), Bytes{}}};
    static_cast<void>(write_entries(entry));
  }

  std::map<InternalKey, Bytes, InternalKeyLess> all_versions() const {
    const auto snapshot = state_snapshot();
    std::map<InternalKey, Bytes, InternalKeyLess> versions;
    const auto add_memtable = [&versions](const auto& table) {
      for (auto& [key, value] : table->entries())
        versions.try_emplace(std::move(key), std::move(value));
    };
    const auto add_sst = [&versions](const TableSlot& slot) {
      for (auto iterator = slot.table->iter(); iterator->valid();
           iterator->next())
        versions.try_emplace(
            iterator->key(),
            Bytes(iterator->value().begin(), iterator->value().end()));
    };
    add_memtable(snapshot->mutable_memtable.table);
    for (const auto& immutable : snapshot->immutable_memtables) {
      add_memtable(immutable.table);
    }
    for (const auto& table : snapshot->l0_tables) add_sst(table);
    for (const auto& table : snapshot->l1_tables) add_sst(table);
    return versions;
  }

  static bool user_key_in_table(const TableSlot& slot, ByteView key) {
    return !bytes_less(key, slot.first_key.user_key()) &&
           !bytes_less(slot.last_key.user_key(), key);
  }

  std::optional<Bytes> get_at(ByteView key, std::uint64_t read_timestamp) const {
    require_open();
    validate_key(key);
    counters.get_ops.fetch_add(1, std::memory_order_relaxed);
    const auto snapshot = state_snapshot();

    auto consider = [&](std::optional<Bytes> found)
        -> std::optional<std::optional<Bytes>> {
      if (!found) return std::nullopt;
      counters.get_hits.fetch_add(1, std::memory_order_relaxed);
      if (found->empty()) return std::optional<Bytes>{};
      return found;
    };

    if (auto decided =
            consider(snapshot->mutable_memtable.table->get(key, read_timestamp))) {
      return *decided;
    }
    for (const auto& immutable : snapshot->immutable_memtables) {
      if (auto decided =
              consider(immutable.table->get(key, read_timestamp))) {
        return *decided;
      }
    }

    auto probe_sst = [&](const TableSlot& slot)
        -> std::optional<std::optional<Bytes>> {
      if (!user_key_in_table(slot, key) || !slot.table->may_contain(key)) {
        return std::nullopt;
      }
      return consider(slot.table->get(key, read_timestamp, block_cache.get(),
                                      slot.id));
    };

    for (const auto& table : snapshot->l0_tables) {
      if (auto decided = probe_sst(table)) return *decided;
    }
    for (const auto& table : snapshot->l1_tables) {
      if (auto decided = probe_sst(table)) return *decided;
    }
    return std::nullopt;
  }

  std::optional<Bytes> get(ByteView key) const {
    return get_at(key, timestamps.claimed());
  }

  DbIterator scan_at(const KeyBound& lower, const KeyBound& upper,
                     std::uint64_t read_timestamp) const {
    require_open();
    const auto versions = all_versions();
    std::map<Bytes, Bytes> visible;
    for (const auto& [internal_key, value] : versions) {
      if (internal_key.timestamp() > read_timestamp) continue;
      Bytes user_key(internal_key.user_key().begin(),
                     internal_key.user_key().end());
      visible.try_emplace(std::move(user_key), value);
    }

    const auto in_bounds = [&lower, &upper](const Bytes& key) {
      if (lower.kind == BoundKind::included && key < lower.key) return false;
      if (lower.kind == BoundKind::excluded && key <= lower.key) return false;
      if (upper.kind == BoundKind::included && key > upper.key) return false;
      if (upper.kind == BoundKind::excluded && key >= upper.key) return false;
      return true;
    };
    std::vector<std::pair<Bytes, Bytes>> output;
    for (auto& [key, value] : visible) {
      if (!value.empty() && in_bounds(key)) {
        output.emplace_back(std::move(key), std::move(value));
      }
    }
    counters.scan_ops.fetch_add(1, std::memory_order_relaxed);
    counters.scan_keys.fetch_add(output.size(), std::memory_order_relaxed);
    return DbIterator(std::move(output));
  }

  DbIterator scan(const KeyBound& lower, const KeyBound& upper) const {
    return scan_at(lower, upper, timestamps.claimed());
  }

  std::string dump_structure() const {
    const auto snapshot = state_snapshot();
    std::ostringstream out;
    out << "mutable_memtable=" << snapshot->mutable_memtable.id
        << " entries=" << snapshot->mutable_memtable.table->entries().size()
        << '\n';
    out << "immutable_memtables=" << snapshot->immutable_memtables.size()
        << '\n';
    for (std::size_t index = 0;
         index < snapshot->immutable_memtables.size(); ++index) {
      const auto& immutable = snapshot->immutable_memtables[index];
      out << "immutable[" << index << "]=" << immutable.id
          << " entries=" << immutable.table->entries().size() << '\n';
    }
    out << "l0_tables=" << snapshot->l0_tables.size() << '\n';
    out << "l1_tables=" << snapshot->l1_tables.size() << '\n';
    for (std::size_t index = 0; index < snapshot->l1_tables.size(); ++index) {
      out << "l1[" << index << "]=" << snapshot->l1_tables[index].id << '\n';
    }
    const auto versions = all_versions();
    std::size_t keys = 0;
    Bytes previous;
    for (const auto& [key, value] : versions) {
      static_cast<void>(value);
      Bytes user(key.user_key().begin(), key.user_key().end());
      if (user != previous) {
        ++keys;
        previous = std::move(user);
      }
    }
    out << "versions=" << versions.size() << '\n';
    out << "keys=" << keys << '\n';
    out << "active_snapshots=" << active_readers() << '\n';
    return out.str();
  }

  DbMetrics metrics() const {
    const auto snapshot = state_snapshot();
    DbMetrics result;
    result.write_ops = counters.write_ops.load(std::memory_order_relaxed);
    result.write_user_bytes =
        counters.write_user_bytes.load(std::memory_order_relaxed);
    result.wal_bytes = counters.wal_bytes.load(std::memory_order_relaxed);
    result.get_ops = counters.get_ops.load(std::memory_order_relaxed);
    result.get_hits = counters.get_hits.load(std::memory_order_relaxed);
    result.scan_ops = counters.scan_ops.load(std::memory_order_relaxed);
    result.scan_keys = counters.scan_keys.load(std::memory_order_relaxed);
    result.freeze_count = counters.freeze_count.load(std::memory_order_relaxed);
    result.flush_count = counters.flush_count.load(std::memory_order_relaxed);
    result.flush_bytes = counters.flush_bytes.load(std::memory_order_relaxed);
    result.compaction_count =
        counters.compaction_count.load(std::memory_order_relaxed);
    result.compaction_input_bytes =
        counters.compaction_input_bytes.load(std::memory_order_relaxed);
    result.compaction_output_bytes =
        counters.compaction_output_bytes.load(std::memory_order_relaxed);
    result.sst_raw_bytes =
        counters.sst_raw_bytes.load(std::memory_order_relaxed);
    result.sst_stored_bytes =
        counters.sst_stored_bytes.load(std::memory_order_relaxed);
    if (block_cache) {
      result.block_cache_hits = block_cache->hits();
      result.block_cache_misses = block_cache->misses();
    }
    result.immutable_memtables = snapshot->immutable_memtables.size();
    result.l0_tables = snapshot->l0_tables.size();
    result.l1_tables = snapshot->l1_tables.size();
    return result;
  }
};

std::shared_ptr<DB> DB::Open(const std::filesystem::path& path,
                                       Options options) {
  if (options.target_sst_size == 0) {
    throw Error("target_sst_size must be greater than zero");
  }
  std::filesystem::create_directories(path);
  auto database = std::shared_ptr<DB>(
      new DB(std::make_unique<Impl>(path, std::move(options))));
  database->impl_->recover_manifest();
  database->impl_->recover_wals();
  database->impl_->start_workers();
  return database;
}

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DB::~DB() {
  try {
    Close();
  } catch (...) {
  }
}

std::optional<Bytes> DB::Get(ByteView key) const {
  return impl_->get(key);
}

std::optional<Bytes> DB::Get(std::string_view key) const {
  return Get(as_bytes(key));
}

void DB::Put(ByteView key, ByteView value) {
  if (impl_->options.serializable) {
    auto transaction = NewTransaction();
    transaction->Put(key, value);
    transaction->Commit();
    return;
  }
  impl_->put(key, value);
}

void DB::Put(std::string_view key, std::string_view value) {
  Put(as_bytes(key), as_bytes(value));
}

void DB::Delete(ByteView key) {
  if (impl_->options.serializable) {
    auto transaction = NewTransaction();
    transaction->Delete(key);
    transaction->Commit();
    return;
  }
  impl_->erase(key);
}

void DB::Delete(std::string_view key) { Delete(as_bytes(key)); }

void DB::WriteBatch(std::span<const WriteBatchRecord> batch) {
  impl_->require_open();
  if (impl_->options.serializable) {
    auto transaction = NewTransaction();
    for (const auto& record : batch) {
      std::visit(
          [&](const auto& item) {
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                         PutRecord>)
              transaction->Put(item.key, item.value);
            else
              transaction->Delete(item.key);
          },
          record);
    }
    transaction->Commit();
    return;
  }
  std::vector<std::pair<Bytes, Bytes>> entries;
  entries.reserve(batch.size());
  for (const auto& record : batch) {
    std::visit(
        [&](const auto& item) {
          validate_key(item.key);
          Bytes value;
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       PutRecord>) {
            validate_value(item.value);
            value = item.value;
          }
          entries.emplace_back(item.key, value);
        },
        record);
  }
  static_cast<void>(impl_->write_entries(entries));
}

DbIterator DB::Scan(KeyBound lower, KeyBound upper) const {
  return impl_->scan(lower, upper);
}

void DB::Sync() {
  impl_->require_open();
  const auto snapshot = impl_->state_snapshot();
  if (snapshot->mutable_memtable.wal) snapshot->mutable_memtable.wal->sync();
  for (const auto& immutable : snapshot->immutable_memtables) {
    if (immutable.wal) immutable.wal->sync();
  }
}

void DB::Close() {
  if (impl_->closed.load(std::memory_order_acquire)) return;
  impl_->stop_worker();
  Sync();
  if (!impl_->options.enable_wal) impl_->force_flush();
  impl_->closed.store(true, std::memory_order_release);
}

void DB::ForceFreezeMemTable() { impl_->force_freeze(); }

void DB::ForceFlush() {
  impl_->force_flush();
}

void DB::ForceFullCompaction() {
  impl_->force_flush();
  impl_->force_full_compaction();
}

void DB::AddCompactionFilter(Bytes prefix) {
  impl_->require_open();
  if (prefix.empty()) throw Error("compaction filter prefix must not be empty");
  std::lock_guard lock(impl_->filters_mutex);
  impl_->compaction_filters.push_back(std::move(prefix));
}

struct Transaction::Impl {
  Impl(std::shared_ptr<DB> database_arg, std::uint64_t read_timestamp_arg)
      : database(std::move(database_arg)),
        read_timestamp(read_timestamp_arg) {}

  std::shared_ptr<DB> database;
  std::uint64_t read_timestamp{};
  bool committed{};
  bool registered{true};
  std::map<Bytes, Bytes> workspace;
  mutable std::set<Bytes> read_set;
};

std::shared_ptr<Transaction> DB::NewTransaction() {
  impl_->require_open();
  impl_->timestamps.wait_quiesced();
  const auto timestamp = impl_->timestamps.visible();
  impl_->add_reader(timestamp);
  return std::shared_ptr<Transaction>(new Transaction(
      std::make_unique<Transaction::Impl>(shared_from_this(), timestamp)));
}

std::string DB::DumpStructure() const {
  impl_->require_open();
  return impl_->dump_structure();
}

DbMetrics DB::Metrics() const {
  impl_->require_open();
  return impl_->metrics();
}

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() {
  if (impl_ && impl_->registered)
    impl_->database->impl_->remove_reader(impl_->read_timestamp);
}

std::optional<Bytes> Transaction::Get(ByteView key) const {
  if (impl_->committed) throw Error("transaction is already committed");
  validate_key(key);
  const Bytes owned_key(key.begin(), key.end());
  impl_->read_set.insert(owned_key);
  if (const auto local = impl_->workspace.find(owned_key);
      local != impl_->workspace.end()) {
    return local->second.empty() ? std::nullopt
                                 : std::optional<Bytes>(local->second);
  }
  return impl_->database->impl_->get_at(key, impl_->read_timestamp);
}

std::optional<Bytes> Transaction::Get(std::string_view key) const {
  return Get(as_bytes(key));
}

DbIterator Transaction::Scan(KeyBound lower, KeyBound upper) const {
  if (impl_->committed) throw Error("transaction is already committed");
  auto base =
      impl_->database->impl_->scan_at(lower, upper, impl_->read_timestamp);
  std::map<Bytes, Bytes> visible;
  while (base.valid()) {
    visible.emplace(Bytes(base.key().begin(), base.key().end()),
                    Bytes(base.value().begin(), base.value().end()));
    base.next();
  }
  const auto in_bounds = [&lower, &upper](const Bytes& key) {
    if (lower.kind == BoundKind::included && key < lower.key) return false;
    if (lower.kind == BoundKind::excluded && key <= lower.key) return false;
    if (upper.kind == BoundKind::included && key > upper.key) return false;
    if (upper.kind == BoundKind::excluded && key >= upper.key) return false;
    return true;
  };
  for (const auto& [key, value] : impl_->workspace) {
    if (!in_bounds(key)) continue;
    if (value.empty())
      visible.erase(key);
    else
      visible[key] = value;
  }
  std::vector<std::pair<Bytes, Bytes>> output(visible.begin(), visible.end());
  for (const auto& [key, value] : output) {
    static_cast<void>(value);
    impl_->read_set.insert(key);
  }
  return DbIterator(std::move(output));
}

void Transaction::Put(ByteView key, ByteView value) {
  if (impl_->committed) throw Error("transaction is already committed");
  validate_key(key);
  validate_value(value);
  impl_->workspace[owned(key)] = owned(value);
}

void Transaction::Put(std::string_view key, std::string_view value) {
  Put(as_bytes(key), as_bytes(value));
}

void Transaction::Delete(ByteView key) {
  if (impl_->committed) throw Error("transaction is already committed");
  validate_key(key);
  impl_->workspace[owned(key)] = {};
}

void Transaction::Delete(std::string_view key) { Delete(as_bytes(key)); }

void Transaction::Commit() {
  if (impl_->committed) throw Error("transaction is already committed");
  auto& database = *impl_->database->impl_;
  std::lock_guard commit_lock(database.commit_mutex);

  if (impl_->workspace.empty()) {
    if (impl_->registered) {
      database.remove_reader(impl_->read_timestamp);
      impl_->registered = false;
    }
    impl_->committed = true;
    return;
  }

  if (database.options.serializable) {
    for (auto committed = database.committed_writes.upper_bound(
             impl_->read_timestamp);
         committed != database.committed_writes.end(); ++committed) {
      for (const auto& key : impl_->read_set) {
        if (committed->second.contains(key))
          throw Error("transaction serializable validation conflict");
      }
    }
  }

  const auto versions = database.all_versions();
  for (const auto& [key, value] : impl_->workspace) {
    static_cast<void>(value);
    const auto newest =
        versions.lower_bound(InternalKey(key, kMaxTimestamp));
    if (newest != versions.end() &&
        std::equal(newest->first.user_key().begin(),
                   newest->first.user_key().end(), key.begin(), key.end()) &&
        newest->first.timestamp() > impl_->read_timestamp) {
      throw Error("transaction write-write conflict");
    }
  }

  std::vector<std::pair<Bytes, Bytes>> entries(impl_->workspace.begin(),
                                               impl_->workspace.end());
  const auto commit_timestamp = database.write_entries_locked(entries);
  if (database.options.serializable) {
    std::set<Bytes> write_set;
    for (const auto& [key, value] : impl_->workspace) {
      static_cast<void>(value);
      write_set.insert(key);
    }
    database.committed_writes.emplace(commit_timestamp, std::move(write_set));
  }
  if (impl_->registered) {
    database.remove_reader(impl_->read_timestamp);
    impl_->registered = false;
  }
  impl_->committed = true;
  const auto gc_watermark = database.watermark();
  database.committed_writes.erase(
      database.committed_writes.begin(),
      database.committed_writes.lower_bound(gc_watermark));
}

std::uint64_t Transaction::read_timestamp() const noexcept {
  return impl_->read_timestamp;
}

bool Transaction::committed() const noexcept { return impl_->committed; }

}  // namespace hermesdb
