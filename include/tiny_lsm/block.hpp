#pragma once

#include "tiny_lsm/iterator.hpp"

#include <memory>

namespace tiny_lsm {

class Block {
 public:
  [[nodiscard]] static std::shared_ptr<Block> decode(Bytes encoded);
  [[nodiscard]] Bytes encode() const;
  [[nodiscard]] std::size_t size() const noexcept { return offsets_.size(); }
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] ByteView data() const noexcept { return data_; }
  [[nodiscard]] std::uint16_t offset(std::size_t index) const;

 private:
  friend class BlockBuilder;
  Block(Bytes data, std::vector<std::uint16_t> offsets);
  Bytes data_;
  std::vector<std::uint16_t> offsets_;
};

class BlockBuilder {
 public:
  explicit BlockBuilder(std::size_t target_size);
  [[nodiscard]] bool add(ByteView key, ByteView value);
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] std::size_t estimated_size() const noexcept;
  [[nodiscard]] std::shared_ptr<Block> finish();

 private:
  std::size_t target_size_;
  Bytes data_;
  std::vector<std::uint16_t> offsets_;
  Bytes first_key_;
};

class BlockIterator final : public StorageIterator {
 public:
  explicit BlockIterator(std::shared_ptr<const Block> block);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] ByteView key() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  void seek_to_first();
  void seek(ByteView target);

 private:
  void decode_entry(std::size_t index);
  std::shared_ptr<const Block> block_;
  std::size_t index_{};
  Bytes key_;
  Bytes first_key_;
  ByteView value_;
  bool valid_{};
};

}  // namespace tiny_lsm
