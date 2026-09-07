#include "tiny_lsm/db.hpp"

#include "tiny_lsm/memtable.hpp"
#include "tiny_lsm/transaction.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <shared_mutex>

namespace tiny_lsm {
namespace {

[[noreturn]] void course_todo(std::string_view label) {
  throw Error("TODO(" + std::string(label) + " is not implemented");
}

}  // namespace

KeyBound KeyBound::Included(std::string_view key) {
  // TODO(week1-day2): construct an inclusive scan bound.
  (void)key;
  course_todo("week1-day2): KeyBound::Included");
}

KeyBound KeyBound::Excluded(std::string_view key) {
  // TODO(week1-day2): construct an exclusive scan bound.
  (void)key;
  course_todo("week1-day2): KeyBound::Excluded");
}

DbIterator::DbIterator() = default;
DbIterator::DbIterator(std::vector<std::pair<Bytes, Bytes>> entries)
    : entries_(std::move(entries)) {}
bool DbIterator::valid() const noexcept {
  // TODO(week1-day2): report whether the iterator points at an entry.
  (void)index_;
  return false;
}
ByteView DbIterator::key() const {
  // TODO(week1-day2): return the current key view.
  course_todo("week1-day2): DbIterator::key");
}
ByteView DbIterator::value() const {
  // TODO(week1-day2): return the current value view.
  course_todo("week1-day2): DbIterator::value");
}
void DbIterator::next() {
  // TODO(week1-day2): advance the iterator.
  course_todo("week1-day2): DbIterator::next");
}

struct MiniLsm::Impl {
  struct MemTableSlot {
    std::uint64_t id{};
    std::shared_ptr<MemTable> table;
  };

  struct State {
    // Invariant: exactly one mutable memtable receives all new writes.
    MemTableSlot mutable_memtable;
    // Invariant: immutable memtables are ordered newest first.
    std::vector<MemTableSlot> immutable_memtables;
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

  void put(ByteView, ByteView) {
    // TODO(week1-day1): validate, write to the current mutable memtable while
    // preventing an in-flight freeze, then invoke maybe_freeze.
    course_todo("week1-day1): MiniLsm::Put");
  }

  void erase(ByteView) {
    // TODO(week1-day1): represent deletion by writing an empty tombstone.
    course_todo("week1-day1): MiniLsm::Delete");
  }

  std::optional<Bytes> get(ByteView) const {
    // TODO(week1-day1): search mutable first, then immutable newest-to-oldest;
    // stop at the first match and hide an empty tombstone from the caller.
    course_todo("week1-day1): MiniLsm::Get");
  }

  void maybe_freeze(const std::shared_ptr<MemTable>&) {
    // TODO(week1-day1): freeze only when target_sst_size is reached, and
    // re-check both identity and size while holding transition locks.
    course_todo("week1-day1): size-triggered freeze");
  }

  void force_freeze() {
    // TODO(week1-day1): freeze the expected non-empty mutable memtable once;
    // concurrent callers must not publish empty immutable memtables.
    course_todo("week1-day1): forced freeze");
  }

  void freeze_locked() {
    // TODO(week1-day1): copy State, prepend the old mutable memtable to the
    // immutable list, install a fresh mutable memtable, and publish atomically.
    course_todo("week1-day1): freeze state transition");
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
DbIterator MiniLsm::Scan(KeyBound, KeyBound) const {
  // TODO(week1-day2): implement range scans.
  course_todo("week1-day2): MiniLsm::Scan");
}
void MiniLsm::Sync() {
  // TODO(week2-day6): sync persistent state.
  course_todo("week2-day6): MiniLsm::Sync");
}
void MiniLsm::Close() {
  impl_->closed.store(true, std::memory_order_release);
}
void MiniLsm::ForceFlush() {
  // TODO(week1-day6): flush an immutable memtable to an SST.
  course_todo("week1-day6): MiniLsm::ForceFlush");
}
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
  // TODO(week1-day1): expose mutable/immutable IDs for course diagnostics.
  course_todo("week1-day1): MiniLsm::DumpStructure");
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
