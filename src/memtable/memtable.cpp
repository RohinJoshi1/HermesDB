#include "tiny_lsm/memtable.hpp"

namespace tiny_lsm {

void MemTable::put(ByteView, ByteView) {
  // TODO(week1-day1): insert/overwrite under mutex_ and update
  // approximate_size_. Empty values are tombstones and must be retained.
  throw Error("TODO(week1-day1): MemTable::put");
}

void MemTable::put(std::string_view key, std::string_view value) {
  put(as_bytes(key), as_bytes(value));
}

std::optional<Bytes> MemTable::get(ByteView) const {
  // TODO(week1-day1): look up the owned user key under a shared lock.
  throw Error("TODO(week1-day1): MemTable::get");
}

std::optional<Bytes> MemTable::get(std::string_view key) const {
  return get(as_bytes(key));
}

std::vector<std::pair<Bytes, Bytes>> MemTable::entries() const {
  // TODO(week1-day1): copy entries_ in map order under a shared lock.
  throw Error("TODO(week1-day1): MemTable::entries");
}

std::size_t MemTable::approximate_size() const {
  // TODO(week1-day1): read approximate_size_ under a shared lock.
  (void)approximate_size_;
  throw Error("TODO(week1-day1): MemTable::approximate_size");
}

bool MemTable::empty() const {
  // TODO(week1-day1): inspect entries_ under a shared lock.
  throw Error("TODO(week1-day1): MemTable::empty");
}

}  // namespace tiny_lsm
