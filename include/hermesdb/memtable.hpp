#pragma once

#include "hermesdb/common.hpp"
#include "hermesdb/iterator.hpp"

#include <map>
#include <optional>
#include <shared_mutex>
#include <span>

namespace hermesdb {

class MemTable {
 public:
  void put(ByteView key, ByteView value);
  void put(std::string_view key, std::string_view value);
  void put(ByteView key, std::uint64_t timestamp, ByteView value);
  void put(std::string_view key, std::uint64_t timestamp, ByteView value);
  void erase(ByteView key, std::uint64_t timestamp);
  void erase(std::string_view key, std::uint64_t timestamp);
  void put_batch(
      std::span<const std::pair<Bytes, Bytes>> entries);
  void put_batch(std::span<const KeyValue> entries);
  [[nodiscard]] std::optional<Bytes> get(ByteView key) const;
  [[nodiscard]] std::optional<Bytes> get(std::string_view key) const;
  [[nodiscard]] std::optional<Bytes> get(ByteView key,
                                         std::uint64_t read_timestamp) const;
  [[nodiscard]] std::optional<Bytes> get(std::string_view key,
                                         std::uint64_t read_timestamp) const;
  [[nodiscard]] std::vector<KeyValue> entries() const;
  [[nodiscard]] IteratorPtr iter() const;
  [[nodiscard]] IteratorPtr scan(const InternalKey& lower,
                                 const InternalKey& upper) const;
  [[nodiscard]] std::size_t approximate_size() const;
  [[nodiscard]] bool empty() const;

 private:
  mutable std::shared_mutex mutex_;
  std::map<InternalKey, Bytes, InternalKeyLess> entries_;
  std::size_t approximate_size_{};
};

}  // namespace hermesdb
