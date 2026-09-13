#include "tiny_lsm/memtable.hpp"

#include <mutex>

namespace tiny_lsm {

void MemTable::put(ByteView key_view, ByteView value_view) {
  Bytes key(key_view.begin(), key_view.end());
  Bytes value(value_view.begin(), value_view.end());
  std::unique_lock lock(mutex_);

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    approximate_size_ -= it->second.size();
    approximate_size_ += value.size();
    it->second = std::move(value);
  } else {
    approximate_size_ += key.size() + value.size();
    entries_.emplace(std::move(key), std::move(value));
  }
}

void MemTable::put(std::string_view key, std::string_view value) {
  put(as_bytes(key), as_bytes(value));
}

std::optional<Bytes> MemTable::get(ByteView key_view) const {
  Bytes key(key_view.begin(), key_view.end());
  std::shared_lock lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

std::optional<Bytes> MemTable::get(std::string_view key) const {
  return get(as_bytes(key));
}

std::vector<std::pair<Bytes, Bytes>> MemTable::entries() const {
  std::shared_lock lock(mutex_);
  return {entries_.begin(), entries_.end()};
}

std::size_t MemTable::approximate_size() const {
  std::shared_lock lock(mutex_);
  return approximate_size_;
}

bool MemTable::empty() const {
  std::shared_lock lock(mutex_);
  return entries_.empty();
}

}  // namespace tiny_lsm
