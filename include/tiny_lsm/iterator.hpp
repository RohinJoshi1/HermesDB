#pragma once

#include "tiny_lsm/key.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tiny_lsm {

class StorageIterator {
 public:
  virtual ~StorageIterator() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalKey& key() const = 0;
  [[nodiscard]] virtual ByteView value() const = 0;
  virtual void next() = 0;
};

using IteratorPtr = std::unique_ptr<StorageIterator>;
using KeyValue = std::pair<InternalKey, Bytes>;

class VectorIterator final : public StorageIterator {
 public:
  explicit VectorIterator(std::vector<KeyValue> entries);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] const InternalKey& key() const override;
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
  [[nodiscard]] ByteView value() const override;
  void next() override;

 private:
  void select_current();
  std::vector<IteratorPtr> children_;
  std::optional<std::size_t> current_;
};

}  // namespace tiny_lsm
