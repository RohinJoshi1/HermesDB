#pragma once

#include "tiny_lsm/block.hpp"

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

namespace tiny_lsm {

struct BlockMeta {
  std::uint32_t offset{};
  Bytes first_key;
  Bytes last_key;
};

class BloomFilter {
 public:
  static BloomFilter Build(std::span<const std::uint32_t> key_hashes,
                           std::size_t bits_per_key);
  static std::size_t BitsPerKey(std::size_t entries,
                                double false_positive_rate = 0.01);
  static BloomFilter Decode(ByteView encoded);

  [[nodiscard]] bool MayContain(std::uint32_t key_hash) const noexcept;
  [[nodiscard]] Bytes Encode() const;
  [[nodiscard]] std::size_t bit_count() const noexcept {
    return bits_.size() * 8U;
  }

 private:
  BloomFilter(Bytes bits, std::uint8_t hash_count)
      : bits_(std::move(bits)), hash_count_(hash_count) {}
  Bytes bits_;
  std::uint8_t hash_count_{};
};

class BlockCache {
 public:
  explicit BlockCache(std::size_t capacity);
  [[nodiscard]] std::shared_ptr<const Block> Get(std::uint64_t table_id,
                                                  std::size_t block_index);
  void Insert(std::uint64_t table_id, std::size_t block_index,
              std::shared_ptr<const Block> block);

 private:
  struct Key {
    std::uint64_t table_id{};
    std::size_t block_index{};
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept;
  };
  using Entry = std::pair<Key, std::shared_ptr<const Block>>;

  std::size_t capacity_;
  std::list<Entry> entries_;
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
  std::mutex mutex_;
};

class Table : public std::enable_shared_from_this<Table> {
 public:
  [[nodiscard]] static std::shared_ptr<Table> open(Bytes encoded);
  [[nodiscard]] ByteView bytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::vector<BlockMeta>& block_meta() const noexcept {
    return meta_;
  }
  [[nodiscard]] std::size_t num_blocks() const noexcept { return meta_.size(); }
  [[nodiscard]] ByteView first_key() const noexcept;
  [[nodiscard]] ByteView last_key() const noexcept;
  [[nodiscard]] bool may_contain(ByteView user_key) const noexcept;
  [[nodiscard]] std::optional<Bytes> get(ByteView key) const;
  [[nodiscard]] std::shared_ptr<const Block> read_block(std::size_t index) const;
  [[nodiscard]] std::shared_ptr<const Block> read_block_cached(
      std::size_t index, std::uint64_t table_id, BlockCache& cache) const;
  [[nodiscard]] std::size_t find_block(ByteView key) const;
  [[nodiscard]] IteratorPtr iter() const;
  [[nodiscard]] IteratorPtr iter_from(ByteView key) const;

 private:
  Table(Bytes bytes, std::vector<BlockMeta> meta, std::uint32_t meta_offset,
        BloomFilter bloom);
  Bytes bytes_;
  std::vector<BlockMeta> meta_;
  std::uint32_t meta_offset_{};
  BloomFilter bloom_;
};

class TableBuilder {
 public:
  explicit TableBuilder(std::size_t block_size);
  void add(ByteView key, ByteView value);
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] Bytes finish();

 private:
  void finish_block();
  std::size_t block_size_;
  BlockBuilder block_;
  Bytes data_;
  std::vector<BlockMeta> meta_;
  std::optional<Bytes> first_key_;
  std::optional<Bytes> last_key_;
  std::optional<Bytes> previous_key_;
  std::vector<std::uint32_t> key_hashes_;
};

class TableIterator final : public StorageIterator {
 public:
  explicit TableIterator(std::shared_ptr<const Table> table);
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] ByteView key() const override;
  [[nodiscard]] ByteView value() const override;
  void next() override;
  void seek_to_first();
  void seek(ByteView target);

 private:
  void open_block(std::size_t index, const ByteView* target);
  std::shared_ptr<const Table> table_;
  std::size_t block_index_{};
  std::unique_ptr<BlockIterator> block_iter_;
};

}  // namespace tiny_lsm
