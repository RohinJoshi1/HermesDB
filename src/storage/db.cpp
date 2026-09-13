#include "tiny_lsm/db.hpp"

#include "tiny_lsm/iterator.hpp"
#include "tiny_lsm/memtable.hpp"
#include "tiny_lsm/table.hpp"
#include "tiny_lsm/transaction.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>

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
  };

  std::filesystem::path path;
  LsmStorageOptions options;
  mutable std::shared_mutex state_mutex;
  // Serializes state transitions such as freezing the mutable memtable.
  std::mutex state_change_mutex;
  std::shared_ptr<const State> state;
  std::uint64_t next_memtable_id{1};
  std::atomic<bool> closed{false};

  Impl(std::filesystem::path path_arg, LsmStorageOptions options_arg)
      : path(std::move(path_arg)), options(std::move(options_arg)) {
    auto initial = std::make_shared<State>();
    initial->mutable_memtable = {0, std::make_shared<MemTable>()};
    state = std::move(initial);
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
  }

  void freeze_locked() {
    auto new_state = std::make_shared<State>(*state);
    new_state->immutable_memtables.insert(
        new_state->immutable_memtables.begin(), state->mutable_memtable);
    new_state->mutable_memtable = {
        next_memtable_id++, std::make_shared<MemTable>()};
    state = std::move(new_state);
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
    if (entries.empty()) throw Error("cannot flush an empty memtable");
    TableBuilder builder(options.block_size);
    for (const auto& [key, value] : entries) {
      builder.add(key, value);
    }
    Bytes encoded = builder.finish();
    const auto temporary = path / (std::to_string(source.id) + ".sst.tmp");
    const auto destination = path / (std::to_string(source.id) + ".sst");
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw Error("failed to create SST");
      output.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
      output.flush();
      if (!output) throw Error("failed to write SST");
    }
    std::filesystem::rename(temporary, destination);
    auto table = Table::open(std::move(encoded));

    std::unique_lock change_lock(state_change_mutex);
    std::unique_lock state_lock(state_mutex);
    if (state->immutable_memtables.empty() ||
        state->immutable_memtables.back().id != source.id) {
      throw Error("immutable memtable changed during flush");
    }
    auto next = std::make_shared<State>(*state);
    next->immutable_memtables.pop_back();
    next->l0_tables.insert(next->l0_tables.begin(),
                           TableSlot{source.id, std::move(table)});
    state = std::move(next);
    return true;
  }

  void force_flush() {
    require_open();
    force_freeze();
    while (flush_oldest_immutable()) {
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
  impl_->closed.store(true, std::memory_order_release);
}
void MiniLsm::ForceFlush() { impl_->force_flush(); }
void MiniLsm::ForceFullCompaction() {
  // TODO(week2-day1): implement full compaction.
  course_todo("week2-day1): MiniLsm::ForceFullCompaction");
}
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
