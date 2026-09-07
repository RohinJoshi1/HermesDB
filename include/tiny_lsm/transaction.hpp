#pragma once

#include "tiny_lsm/db.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace tiny_lsm {

class Transaction {
 public:
  // TODO(week3-day3): implement snapshot reads and scans.
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  [[nodiscard]] std::optional<Bytes> Get(ByteView key) const;
  [[nodiscard]] std::optional<Bytes> Get(std::string_view key) const;
  [[nodiscard]] DbIterator Scan(
      KeyBound lower = KeyBound::Unbounded(),
      KeyBound upper = KeyBound::Unbounded()) const;
  // TODO(week3-day5): implement buffered writes and commit.
  void Put(ByteView key, ByteView value);
  void Put(std::string_view key, std::string_view value);
  void Delete(ByteView key);
  void Delete(std::string_view key);
  void Commit();

  [[nodiscard]] std::uint64_t read_timestamp() const noexcept;
  [[nodiscard]] bool committed() const noexcept;

 private:
  struct Impl;
  explicit Transaction(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class MiniLsm;
};

}  // namespace tiny_lsm
