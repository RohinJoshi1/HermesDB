#pragma once

#include "hermesdb/key.hpp"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hermesdb {

inline constexpr std::size_t kScanBatch = 16;

struct ScanRow {
  InternalKeyView key;
  ByteView value;
};

[[nodiscard]] inline std::size_t leading_same_user(std::span<const ScanRow> rows,
                                                   ByteView user) noexcept {
  std::size_t i = 0;
  if (user.size() == 16 && user.data() != nullptr) {
    while (i < rows.size()) {
      const auto& key = rows[i].key;
      if (key.suffix().empty() && key.prefix().size() == 16) {
        if (!simd::equal16(key.prefix().data(), user.data())) break;
      } else if (!same_user(key, user)) {
        break;
      }
      ++i;
    }
    return i;
  }
  while (i < rows.size() && same_user(rows[i].key, user)) ++i;
  return i;
}

class RowBatch {
 public:
  std::array<ScanRow, kScanBatch> rows{};
  std::uint8_t size{0};
  std::uint8_t pos{0};

  [[nodiscard]] bool empty() const noexcept { return pos >= size; }
  [[nodiscard]] const ScanRow& current() const { return rows[pos]; }
  void pop() { ++pos; }
  void reset() noexcept {
    size = 0;
    pos = 0;
  }

  std::size_t skip_same_user(ByteView user) noexcept {
    if (empty()) return 0;
    const auto n = leading_same_user(
        std::span<const ScanRow>{rows.data() + pos,
                                 static_cast<std::size_t>(size - pos)},
        user);
    pos = static_cast<std::uint8_t>(pos + n);
    return n;
  }
};

class StorageIterator {
 public:
  virtual ~StorageIterator() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalKey& key() const = 0;
  [[nodiscard]] virtual InternalKeyView key_view() const = 0;
  [[nodiscard]] virtual ByteView value() const = 0;
  virtual void next() = 0;
  virtual void skip_current_user();
  // Views in `out` stay valid until the next pull/next/skip/seek on this
  // iterator. Returns the number of rows written.
  virtual std::size_t pull(std::span<ScanRow> out) = 0;
};

using IteratorPtr = std::unique_ptr<StorageIterator>;
using KeyValue = std::pair<InternalKey, Bytes>;

class VectorIterator final : public StorageIterator {
 public:
  explicit VectorIterator(std::vector<KeyValue> entries);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] const InternalKey& key() const override;
  [[nodiscard]] InternalKeyView key_view() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  std::size_t pull(std::span<ScanRow> out) override;

 private:
  std::vector<KeyValue> entries_;
  std::size_t index_{};
};

class MergeIterator final : public StorageIterator {
 public:
  explicit MergeIterator(std::vector<IteratorPtr> children);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] const InternalKey& key() const override;
  [[nodiscard]] InternalKeyView key_view() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  void skip_current_user() override;
  std::size_t pull(std::span<ScanRow> out) override;

 private:
  struct ChildBatch {
    std::array<ScanRow, kScanBatch> rows{};
    std::array<Bytes, kScanBatch> keys{};
    std::array<Bytes, kScanBatch> vals{};
    std::uint8_t size{0};
    std::uint8_t pos{0};
    [[nodiscard]] bool empty() const noexcept { return pos >= size; }
  };

  [[nodiscard]] bool ahead(std::size_t a, std::size_t b) const;
  [[nodiscard]] InternalKeyView child_key(std::size_t child) const;
  [[nodiscard]] ScanRow child_row(std::size_t child) const;
  bool refill_child(std::size_t child);
  void advance_child(std::size_t child);
  void prime();
  [[nodiscard]] std::size_t pick() const;

  std::vector<IteratorPtr> children_;
  std::vector<ChildBatch> batches_;
  Bytes previous_user_;
  std::uint64_t previous_timestamp_{};
  mutable InternalKey key_scratch_;
  std::array<Bytes, kScanBatch> out_keys_{};
  std::array<Bytes, kScanBatch> out_vals_{};
};

class RangeIterator final : public StorageIterator {
 public:
  RangeIterator(IteratorPtr child, std::optional<InternalKey> lower,
                bool lower_inclusive, std::optional<InternalKey> upper,
                bool upper_inclusive);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] const InternalKey& key() const override;
  [[nodiscard]] InternalKeyView key_view() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  std::size_t pull(std::span<ScanRow> out) override;

 private:
  void skip_to_lower();
  [[nodiscard]] bool within_upper() const noexcept;
  IteratorPtr child_;
  std::optional<InternalKey> lower_;
  bool lower_inclusive_{};
  std::optional<InternalKey> upper_;
  bool upper_inclusive_{};
};

}  // namespace hermesdb
