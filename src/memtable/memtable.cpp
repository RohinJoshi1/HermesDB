#include "hermesdb/memtable.hpp"

#include <mutex>

namespace hermesdb {

void MemTable::put(ByteView key, ByteView value) {
  put(key, 0, value);
}

void MemTable::put(ByteView key, std::uint64_t timestamp, ByteView value) {
  InternalKey owned_key(Bytes(key.begin(), key.end()), timestamp);
  Bytes owned(value.begin(), value.end());
  std::unique_lock lock(mutex_);
  const auto it = entries_.find(owned_key);
  if (it != entries_.end()) {
    approximate_size_ -= it->second.size();
    it->second = std::move(owned);
    approximate_size_ += it->second.size();
    return;
  }
  approximate_size_ += owned_key.user_key().size() + 8 + owned.size();
  entries_.emplace(std::move(owned_key), std::move(owned));
}

void MemTable::put(std::string_view key, std::string_view value) {
  put(as_bytes(key), as_bytes(value));
}

void MemTable::put(std::string_view key, std::uint64_t timestamp,
                   ByteView value) {
  put(as_bytes(key), timestamp, value);
}

void MemTable::erase(ByteView key, std::uint64_t timestamp) {
  put(key, timestamp, ByteView{});
}

void MemTable::erase(std::string_view key, std::uint64_t timestamp) {
  erase(as_bytes(key), timestamp);
}

void MemTable::put_batch(
    std::span<const std::pair<Bytes, Bytes>> entries) {
  std::vector<KeyValue> versioned;
  versioned.reserve(entries.size());
  for (const auto& [key, value] : entries)
    versioned.emplace_back(InternalKey(key), value);
  put_batch(versioned);
}

void MemTable::put_batch(std::span<const KeyValue> entries) {
  std::unique_lock lock(mutex_);
  for (const auto& [key, value] : entries) {
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
      approximate_size_ -= found->second.size();
      found->second = value;
      approximate_size_ += found->second.size();
    } else {
      approximate_size_ += key.user_key().size() + 8 + value.size();
      entries_.emplace(key, value);
    }
  }
}

std::optional<Bytes> MemTable::get(ByteView key) const {
  return get(key, kMaxTimestamp);
}

std::optional<Bytes> MemTable::get(ByteView key,
                                   std::uint64_t read_timestamp) const {
  const InternalKey target(Bytes(key.begin(), key.end()), read_timestamp);
  std::shared_lock lock(mutex_);
  const auto it = entries_.lower_bound(target);
  if (it == entries_.end() ||
      !std::equal(it->first.user_key().begin(), it->first.user_key().end(),
                  key.begin(), key.end()))
    return std::nullopt;
  return it->second;
}

std::optional<Bytes> MemTable::get(std::string_view key) const {
  return get(as_bytes(key));
}

std::optional<Bytes> MemTable::get(std::string_view key,
                                   std::uint64_t read_timestamp) const {
  return get(as_bytes(key), read_timestamp);
}

std::vector<KeyValue> MemTable::entries() const {
  std::shared_lock lock(mutex_);
  return {entries_.begin(), entries_.end()};
}

IteratorPtr MemTable::iter() const {
  return std::make_unique<VectorIterator>(entries());
}

IteratorPtr MemTable::scan(const InternalKey& lower,
                           const InternalKey& upper) const {
  std::vector<KeyValue> result;
  std::shared_lock lock(mutex_);
  for (auto it = entries_.lower_bound(lower);
       it != entries_.end() && it->first < upper; ++it) {
    result.push_back(*it);
  }
  return std::make_unique<VectorIterator>(std::move(result));
}

std::size_t MemTable::approximate_size() const {
  std::shared_lock lock(mutex_);
  return approximate_size_;
}

bool MemTable::empty() const {
  std::shared_lock lock(mutex_);
  return entries_.empty();
}

}  // namespace hermesdb
