#include "tiny_lsm/db.hpp"

#include "tiny_lsm/iterator.hpp"
#include "tiny_lsm/memtable.hpp"
#include "tiny_lsm/table.hpp"
#include "tiny_lsm/transaction.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace tiny_lsm {
namespace {

Bytes copy_bytes(std::string_view value) {
  const auto view = as_bytes(value);
  return {view.begin(), view.end()};
}

[[noreturn]] void course_todo(std::string_view label) {
  throw Error("TODO(" + std::string(label) + " is not implemented");
}

}  // namespace

KeyBound KeyBound::Included(std::string_view key) {
  return {BoundKind::included, copy_bytes(key)};
}

KeyBound KeyBound::Excluded(std::string_view key) {
  return {BoundKind::excluded, copy_bytes(key)};
}

DbIterator::DbIterator() = default;
DbIterator::DbIterator(std::vector<std::pair<Bytes, Bytes>> entries)
    : entries_(std::move(entries)) {}
bool DbIterator::valid() const noexcept {
  return index_ < entries_.size();
}
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

struct MiniLsm::Impl {
  struct MemTableSlot {
    std::uint64_t id{};
    std::shared_ptr<MemTable> table;
  };

  struct TableSlot {
    std::uint64_t id{};
    std::shared_ptr<const Table> table;
  };

  struct State {
    // Invariant: exactly one mutable memtable receives all new writes.
    MemTableSlot mutable_memtable;
    // Invariant: immutable memtables are ordered newest first.
    std::vector<MemTableSlot> immutable_memtables;
    // Invariant: L0 SSTs are ordered newest first and may overlap.
    std::vector<TableSlot> l0_tables;
    // Invariant: L1 SSTs are ordered by first key and do not overlap.
    std::vector<TableSlot> l1_tables;
  };

  std::filesystem::path path;
  LsmStorageOptions options;
  mutable std::shared_mutex state_mutex;
  // Serializes state transitions such as freezing the mutable memtable.
  std::mutex state_change_mutex;
  std::shared_ptr<const State> state;
  std::uint64_t next_memtable_id{1};
  std::atomic<bool> closed{false};
  std::mutex flush_cv_mutex_;
  std::condition_variable flush_cv_;
  std::thread flush_thread_;

  Impl(std::filesystem::path path_arg, LsmStorageOptions options_arg)
      : path(std::move(path_arg)), options(std::move(options_arg)) {
    auto initial = std::make_shared<State>();
    initial->mutable_memtable = {0, std::make_shared<MemTable>()};
    state = std::move(initial);
    flush_thread_ = std::thread([this] { flush_loop(); });
  }

  void require_open() const {
    if (closed.load(std::memory_order_acquire)) throw Error("database is closed");
  }

  std::shared_ptr<const State> state_snapshot() const {
    std::shared_lock lock(state_mutex);
    return state;
  }

  void put(ByteView key_view, ByteView value_view) {
    require_open();
    if (key_view.empty()) throw Error("keys must not be empty");
    if (value_view.empty()) {
      throw Error("empty values are reserved for deletion tombstones");
    }

    std::shared_ptr<MemTable> current;
    {
      // Keep the structural read lock until the write finishes so this
      // memtable cannot become immutable while a writer is still using it.
      std::shared_lock lock(state_mutex);
      current = state->mutable_memtable.table;
      current->put(key_view, value_view);
    }
    maybe_freeze(current);
  }

  void erase(ByteView key_view) {
    require_open();
    if (key_view.empty()) throw Error("keys must not be empty");

    std::shared_ptr<MemTable> current;
    {
      std::shared_lock lock(state_mutex);
      current = state->mutable_memtable.table;
      current->put(key_view, ByteView{});
    }
    maybe_freeze(current);
  }

  bool key_in_table(const Table& table, ByteView key) const {
    return !bytes_less(key, table.first_key()) &&
           !bytes_less(table.last_key(), key);
  }

  bool table_overlaps_range(const Table& table, const KeyBound& lower,
                            const KeyBound& upper) const {
    if (lower.kind != BoundKind::unbounded) {
      if (bytes_less(table.last_key(), lower.key)) return false;
      if (lower.kind == BoundKind::excluded &&
          bytes_equal(table.last_key(), lower.key)) {
        return false;
      }
    }
    if (upper.kind != BoundKind::unbounded) {
      if (bytes_less(upper.key, table.first_key())) return false;
      if (upper.kind == BoundKind::excluded &&
          bytes_equal(upper.key, table.first_key())) {
        return false;
      }
    }
    return true;
  }

  std::optional<Bytes> get(ByteView key_view) const {
    require_open();
    if (key_view.empty()) throw Error("keys must not be empty");

    const auto snapshot = state_snapshot();
    auto value = snapshot->mutable_memtable.table->get(key_view);
    if (value) {
      return value->empty() ? std::nullopt : value;
    }
    for (const auto& memtable : snapshot->immutable_memtables) {
      value = memtable.table->get(key_view);
      if (value) {
        // A tombstone is a definitive result. Never continue to an older
        // memtable, or an old value would be resurrected.
        return value->empty() ? std::nullopt : value;
      }
    }
    for (const auto& slot : snapshot->l0_tables) {
      if (!key_in_table(*slot.table, key_view)) continue;
      if (!slot.table->may_contain(key_view)) continue;
      value = slot.table->get(key_view);
      if (value) {
        return value->empty() ? std::nullopt : value;
      }
    }
    const auto l1 = std::lower_bound(
        snapshot->l1_tables.begin(), snapshot->l1_tables.end(), key_view,
        [](const TableSlot& slot, ByteView wanted) {
          return bytes_less(slot.table->last_key(), wanted);
        });
    if (l1 != snapshot->l1_tables.end() &&
        key_in_table(*l1->table, key_view) &&
        l1->table->may_contain(key_view)) {
      value = l1->table->get(key_view);
      if (value) {
        return value->empty() ? std::nullopt : value;
      }
    }
    return std::nullopt;
  }

  DbIterator scan(const KeyBound& lower, const KeyBound& upper) const {
    require_open();
    const auto snapshot = state_snapshot();

    std::vector<IteratorPtr> sources;
    sources.push_back(std::make_unique<VectorIterator>(
        snapshot->mutable_memtable.table->entries()));
    for (const auto& memtable : snapshot->immutable_memtables) {
      sources.push_back(
          std::make_unique<VectorIterator>(memtable.table->entries()));
    }
    for (const auto& slot : snapshot->l0_tables) {
      if (!table_overlaps_range(*slot.table, lower, upper)) continue;
      if (lower.kind == BoundKind::unbounded) {
        sources.push_back(slot.table->iter());
      } else {
        sources.push_back(slot.table->iter_from(lower.key));
      }
    }
    std::vector<std::shared_ptr<const Table>> l1;
    for (const auto& slot : snapshot->l1_tables) {
      if (!table_overlaps_range(*slot.table, lower, upper)) continue;
      l1.push_back(slot.table);
    }
    if (!l1.empty()) {
      auto concat = std::make_unique<ConcatIterator>(std::move(l1));
      if (lower.kind == BoundKind::unbounded) {
        concat->seek_to_first();
      } else {
        concat->seek(lower.key);
      }
      sources.push_back(std::move(concat));
    }

    std::optional<Bytes> lower_key;
    std::optional<Bytes> upper_key;
    if (lower.kind != BoundKind::unbounded) lower_key = lower.key;
    if (upper.kind != BoundKind::unbounded) upper_key = upper.key;

    auto merged = std::make_unique<MergeIterator>(std::move(sources));
    RangeIterator range(std::move(merged), std::move(lower_key),
                        lower.kind == BoundKind::included,
                        std::move(upper_key),
                        upper.kind == BoundKind::included);

    std::vector<std::pair<Bytes, Bytes>> visible;
    while (range.valid()) {
      // MergeIterator resolves duplicate keys first. Only then is it safe to
      // hide a winning tombstone without exposing an older value.
      if (!range.value().empty()) {
        visible.emplace_back(
            Bytes(range.key().begin(), range.key().end()),
            Bytes(range.value().begin(), range.value().end()));
      }
      range.next();
    }
    return DbIterator(std::move(visible));
  }

  void maybe_freeze(const std::shared_ptr<MemTable>& expected) {
    if (expected->approximate_size() < options.target_sst_size) return;

    std::unique_lock change_lock(state_change_mutex);
    std::unique_lock state_lock(state_mutex);
    if (state->mutable_memtable.table != expected ||
        expected->approximate_size() < options.target_sst_size) {
      return;
    }
    freeze_locked();
    notify_flush_thread();
  }

  void force_freeze() {
    require_open();
    const auto snapshot = state_snapshot();
    const auto expected = snapshot->mutable_memtable.table;

    std::unique_lock change_lock(state_change_mutex);
    std::unique_lock state_lock(state_mutex);
    if (state->mutable_memtable.table != expected || expected->empty()) {
      return;
    }
    freeze_locked();
    notify_flush_thread();
  }

  void freeze_locked() {
    auto new_state = std::make_shared<State>(*state);
    new_state->immutable_memtables.insert(
        new_state->immutable_memtables.begin(), state->mutable_memtable);
    new_state->mutable_memtable = {
        next_memtable_id++, std::make_shared<MemTable>()};
    state = std::move(new_state);
  }

  std::uint64_t allocate_file_id() {
    std::unique_lock change_lock(state_change_mutex);
    std::unique_lock state_lock(state_mutex);
    return next_memtable_id++;
  }

  TableSlot write_sst(std::uint64_t id,
                      const std::vector<std::pair<Bytes, Bytes>>& entries) {
    if (entries.empty()) throw Error("cannot write an empty SST");
    TableBuilder builder(options.block_size);
    for (const auto& [key, value] : entries) {
      builder.add(key, value);
    }
    Bytes encoded = builder.finish();
    const auto temporary = path / (std::to_string(id) + ".sst.tmp");
    const auto destination = path / (std::to_string(id) + ".sst");
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw Error("failed to create SST");
      output.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
      output.flush();
      if (!output) throw Error("failed to write SST");
    }
    std::filesystem::rename(temporary, destination);
    return TableSlot{id, Table::open(std::move(encoded))};
  }

  bool flush_oldest_immutable() {
    MemTableSlot source;
    {
      std::unique_lock change_lock(state_change_mutex);
      std::shared_lock state_lock(state_mutex);
      if (state->immutable_memtables.empty()) return false;
      source = state->immutable_memtables.back();
    }

    const auto entries = source.table->entries();
    auto table = write_sst(source.id, entries);

    std::unique_lock change_lock(state_change_mutex);
    std::unique_lock state_lock(state_mutex);
    if (state->immutable_memtables.empty() ||
        state->immutable_memtables.back().id != source.id) {
      throw Error("immutable memtable changed during flush");
    }
    auto next = std::make_shared<State>(*state);
    next->immutable_memtables.pop_back();
    next->l0_tables.insert(next->l0_tables.begin(), std::move(table));
    state = std::move(next);
    return true;
  }

  void trigger_flush() {
    if (closed.load(std::memory_order_acquire)) return;
    const auto snapshot = state_snapshot();
    if (snapshot->immutable_memtables.size() < options.num_memtable_limit) {
      return;
    }
    static_cast<void>(flush_oldest_immutable());
  }

  void notify_flush_thread() { flush_cv_.notify_one(); }

  void flush_loop() {
    while (!closed.load(std::memory_order_acquire)) {
      {
        std::unique_lock lock(flush_cv_mutex_);
        flush_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
          return closed.load(std::memory_order_acquire);
        });
      }
      if (closed.load(std::memory_order_acquire)) break;
      try {
        trigger_flush();
      } catch (...) {
      }
    }
  }

  void stop_flush_thread() {
    closed.store(true, std::memory_order_release);
    notify_flush_thread();
    if (flush_thread_.joinable()) flush_thread_.join();
  }

  void force_flush() {
    require_open();
    force_freeze();
    while (flush_oldest_immutable()) {
    }
  }

  void force_full_compaction() {
    require_open();
    std::vector<TableSlot> captured_l0;
    std::vector<TableSlot> captured_l1;
    {
      std::unique_lock state_lock(state_mutex);
      captured_l0 = state->l0_tables;
      captured_l1 = state->l1_tables;
      if (captured_l0.empty() && captured_l1.empty()) return;
    }

    std::vector<IteratorPtr> sources;
    for (const auto& slot : captured_l0) {
      sources.push_back(slot.table->iter());
    }
    if (!captured_l1.empty()) {
      std::vector<std::shared_ptr<const Table>> tables;
      tables.reserve(captured_l1.size());
      for (const auto& slot : captured_l1) tables.push_back(slot.table);
      auto concat = std::make_unique<ConcatIterator>(std::move(tables));
      concat->seek_to_first();
      sources.push_back(std::move(concat));
    }
    MergeIterator merged(std::move(sources));

    std::vector<TableSlot> outputs;
    std::vector<std::pair<Bytes, Bytes>> current;
    std::size_t current_size = 0;
    const auto flush_builder = [&] {
      if (current.empty()) return;
      outputs.push_back(write_sst(allocate_file_id(), current));
      current.clear();
      current_size = 0;
    };

    while (merged.valid()) {
      if (!merged.value().empty()) {
        const auto key = Bytes(merged.key().begin(), merged.key().end());
        const auto value = Bytes(merged.value().begin(), merged.value().end());
        const std::size_t entry_size = key.size() + value.size();
        if (!current.empty() &&
            current_size + entry_size >= options.target_sst_size) {
          flush_builder();
        }
        current_size += entry_size;
        current.emplace_back(std::move(key), std::move(value));
      }
      merged.next();
    }
    flush_builder();

    std::unordered_set<std::uint64_t> captured_ids;
    for (const auto& slot : captured_l0) captured_ids.insert(slot.id);
    for (const auto& slot : captured_l1) captured_ids.insert(slot.id);

    {
      std::unique_lock change_lock(state_change_mutex);
      std::unique_lock state_lock(state_mutex);
      auto next = std::make_shared<State>(*state);
      std::erase_if(next->l0_tables, [&](const TableSlot& slot) {
        return captured_ids.contains(slot.id);
      });
      std::erase_if(next->l1_tables, [&](const TableSlot& slot) {
        return captured_ids.contains(slot.id);
      });
      next->l1_tables.insert(next->l1_tables.end(), outputs.begin(),
                             outputs.end());
      std::sort(next->l1_tables.begin(), next->l1_tables.end(),
                [](const TableSlot& lhs, const TableSlot& rhs) {
                  return bytes_less(lhs.table->first_key(),
                                    rhs.table->first_key());
                });
      state = std::move(next);
    }
    for (const auto id : captured_ids) {
      std::error_code ignored;
      std::filesystem::remove(path / (std::to_string(id) + ".sst"), ignored);
    }
  }
};

std::shared_ptr<MiniLsm> MiniLsm::Open(const std::filesystem::path& path,
                                      LsmStorageOptions options) {
  if (options.target_sst_size == 0) {
    throw Error("target_sst_size must be greater than zero");
  }
  std::filesystem::create_directories(path);
  return std::shared_ptr<MiniLsm>(
      new MiniLsm(std::make_unique<Impl>(path, std::move(options))));
}

MiniLsm::MiniLsm(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MiniLsm::~MiniLsm() {
  try {
    Close();
  } catch (...) {
  }
}

std::optional<Bytes> MiniLsm::Get(ByteView key) const {
  return impl_->get(key);
}
std::optional<Bytes> MiniLsm::Get(std::string_view key) const {
  return Get(as_bytes(key));
}
void MiniLsm::Put(ByteView key, ByteView value) { impl_->put(key, value); }
void MiniLsm::Put(std::string_view key, std::string_view value) {
  Put(as_bytes(key), as_bytes(value));
}
void MiniLsm::Delete(ByteView key) { impl_->erase(key); }
void MiniLsm::Delete(std::string_view key) { Delete(as_bytes(key)); }
void MiniLsm::ForceFreezeMemTable() { impl_->force_freeze(); }

void MiniLsm::WriteBatch(std::span<const WriteBatchRecord>) {
  // TODO(week2-day7): implement atomic write batches.
  course_todo("week2-day7): MiniLsm::WriteBatch");
}
DbIterator MiniLsm::Scan(KeyBound lower, KeyBound upper) const {
  return impl_->scan(lower, upper);
}
void MiniLsm::Sync() {
  // TODO(week2-day6): sync persistent state.
  course_todo("week2-day6): MiniLsm::Sync");
}
void MiniLsm::Close() {
  impl_->stop_flush_thread();
}
void MiniLsm::ForceFlush() { impl_->force_flush(); }
void MiniLsm::ForceFullCompaction() { impl_->force_full_compaction(); }
void MiniLsm::AddCompactionFilter(Bytes) {
  // TODO(week3-day7): install a compaction filter.
  course_todo("week3-day7): MiniLsm::AddCompactionFilter");
}
std::shared_ptr<Transaction> MiniLsm::NewTransaction() {
  // TODO(week3-day3): create an MVCC transaction.
  course_todo("week3-day3): MiniLsm::NewTransaction");
}
std::string MiniLsm::DumpStructure() const {
  impl_->require_open();
  const auto snapshot = impl_->state_snapshot();

  std::ostringstream output;
  output << "course_checkpoint=week1-day1\n";
  output << "mutable_memtable=" << snapshot->mutable_memtable.id
         << " entries=" << snapshot->mutable_memtable.table->entries().size()
         << '\n';
  output << "immutable_memtables=" << snapshot->immutable_memtables.size()
         << '\n';
  for (std::size_t index = 0;
       index < snapshot->immutable_memtables.size(); ++index) {
    const auto& memtable = snapshot->immutable_memtables[index];
    output << "immutable[" << index << "]=" << memtable.id
           << " entries=" << memtable.table->entries().size() << '\n';
  }
  output << "l0_tables=" << snapshot->l0_tables.size() << '\n';
  output << "l1_tables=" << snapshot->l1_tables.size() << '\n';
  return output.str();
}

struct Transaction::Impl {};
Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() = default;
std::optional<Bytes> Transaction::Get(ByteView) const {
  // TODO(week3-day3): transaction point reads.
  course_todo("week3-day3): Transaction::Get");
}
std::optional<Bytes> Transaction::Get(std::string_view key) const {
  return Get(as_bytes(key));
}
DbIterator Transaction::Scan(KeyBound, KeyBound) const {
  // TODO(week3-day3): transaction range reads.
  course_todo("week3-day3): Transaction::Scan");
}
void Transaction::Put(ByteView, ByteView) {
  // TODO(week3-day5): buffer a transaction write.
  course_todo("week3-day5): Transaction::Put");
}
void Transaction::Put(std::string_view key, std::string_view value) {
  Put(as_bytes(key), as_bytes(value));
}
void Transaction::Delete(ByteView) {
  // TODO(week3-day5): buffer a transaction tombstone.
  course_todo("week3-day5): Transaction::Delete");
}
void Transaction::Delete(std::string_view key) { Delete(as_bytes(key)); }
void Transaction::Commit() {
  // TODO(week3-day5): validate and commit the transaction.
  course_todo("week3-day5): Transaction::Commit");
}
std::uint64_t Transaction::read_timestamp() const noexcept {
  // TODO(week3-day3): return the transaction snapshot timestamp.
  return 0;
}
bool Transaction::committed() const noexcept {
  // TODO(week3-day5): report commit state.
  return false;
}

}  // namespace tiny_lsm
