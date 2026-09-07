#pragma once

#include "tiny_lsm/common.hpp"

#include <map>
#include <optional>
#include <shared_mutex>

namespace tiny_lsm {

class MemTable {
 public:
  // TODO(week1-day1): store or overwrite an ordered user-key entry.
  void put(ByteView key, ByteView value);
  void put(std::string_view key, std::string_view value);
  // TODO(week1-day1): return the value (including an empty tombstone), if any.
  [[nodiscard]] std::optional<Bytes> get(ByteView key) const;
  [[nodiscard]] std::optional<Bytes> get(std::string_view key) const;
  // TODO(week1-day1): return all entries in ascending user-key order.
  [[nodiscard]] std::vector<std::pair<Bytes, Bytes>> entries() const;
  // TODO(week1-day1): maintain key + current-value byte accounting.
  [[nodiscard]] std::size_t approximate_size() const;
  // TODO(week1-day1): report whether the memtable has no entries.
  [[nodiscard]] bool empty() const;

 private:
  mutable std::shared_mutex mutex_;
  std::map<Bytes, Bytes> entries_;
  std::size_t approximate_size_{};
};

}  // namespace tiny_lsm
