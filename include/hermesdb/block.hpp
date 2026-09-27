#pragma once

#include "hermesdb/iterator.hpp"

#include <limits>
#include <memory>

namespace hermesdb {

inline constexpr std::uint16_t kBlockRestartInterval = 16;
inline constexpr std::uint32_t kBlockRestartMagic = 0x52535431;  // RST1

class Block {
 public:
  [[nodiscard]] static std::shared_ptr<Block> decode(Bytes encoded);
  [[nodiscard]] Bytes encode() const;
  [[nodiscard]] std::size_t size() const noexcept { return offsets_.size(); }
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] ByteView data() const noexcept { return data_; }
  [[nodiscard]] std::uint16_t restart_interval() const noexcept {
    return restart_interval_;
  }
  [[nodiscard]] std::size_t restart_base(std::size_t index) const noexcept;
  [[nodiscard]] std::uint16_t offset(std::size_t index) const;
  [[nodiscard]] Bytes first_encoded_key() const;
  [[nodiscard]] InternalKey key_at(std::size_t index,
                                   const Bytes& first_encoded) const;

 private:
  friend class BlockBuilder;
  Block(Bytes data, std::vector<std::uint16_t> offsets,
        std::uint16_t restart_interval);
  Bytes data_;
  std::vector<std::uint16_t> offsets_;
  std::uint16_t restart_interval_{};
};

class BlockBuilder {
 public:
  explicit BlockBuilder(std::size_t target_size);
  [[nodiscard]] bool add(const InternalKey& key, ByteView value);
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] std::size_t estimated_size() const noexcept;
  [[nodiscard]] std::shared_ptr<Block> finish();

 private:
  std::size_t target_size_;
  Bytes data_;
  std::vector<std::uint16_t> offsets_;
  Bytes restart_key_;
};

class BlockIterator final : public StorageIterator {
 public:
  explicit BlockIterator(std::shared_ptr<const Block> block);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] const InternalKey& key() const override;
  [[nodiscard]] InternalKeyView key_view() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  void skip_current_user() override;
  std::size_t pull(std::span<ScanRow> out) override;
  void seek_to_first();
  void seek(const InternalKey& target);

 private:
  void load_restart(std::size_t index) const;
  void decode_entry(std::size_t index) const;
  void refill() const;
  void ensure_batch() const;
  void materialize_key() const;
  std::shared_ptr<const Block> block_;
  mutable std::size_t index_{};
  mutable std::size_t restart_index_{std::numeric_limits<std::size_t>::max()};
  mutable InternalKeyView view_{};
  mutable InternalKey key_;
  mutable bool key_ready_{};
  mutable Bytes restart_key_;
  mutable ByteView value_;
  mutable RowBatch batch_;
  mutable bool valid_{};
};

}  // namespace hermesdb
