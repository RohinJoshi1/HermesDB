#pragma once

#include "hermesdb/key.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace hermesdb {

class StorageIterator {
 public:
  virtual ~StorageIterator() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalKey& key() const = 0;
  [[nodiscard]] virtual InternalKeyView key_view() const = 0;
  [[nodiscard]] virtual ByteView value() const = 0;
  virtual void next() = 0;
  virtual void skip_current_user();
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

 private:
  [[nodiscard]] bool ahead(std::size_t a, std::size_t b) const;
  void push(std::size_t child);
  void pop();
  void sift_up(std::size_t index);
  void sift_down(std::size_t index);
  [[nodiscard]] std::size_t top() const;

  std::vector<IteratorPtr> children_;
  std::vector<std::size_t> heap_;
  Bytes previous_user_;
  std::uint64_t previous_timestamp_{};
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
