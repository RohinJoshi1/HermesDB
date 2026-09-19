#include "hermesdb/table.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hermesdb {
namespace {

void encode_key(Bytes& out, const InternalKey& key) {
  const Bytes encoded = key.encode();
  put_u16(out, narrow_size<std::uint16_t>(encoded.size(), "metadata key"));
  out.insert(out.end(), encoded.begin(), encoded.end());
}

InternalKey decode_key(ByteView bytes, std::size_t& cursor,
                       std::size_t limit) {
  const auto size = read_u16(bytes, cursor);
  cursor += 2;
  if (cursor + size > limit) throw Error("truncated table metadata key");
  const auto result = InternalKey::decode(bytes.subspan(cursor, size));
  cursor += size;
  return result;
}

}  // namespace

BloomFilter BloomFilter::Build(std::span<const std::uint32_t> key_hashes,
                               std::size_t bits_per_key) {
  std::size_t bit_count =
      std::max<std::size_t>(64, key_hashes.size() * bits_per_key);
  bit_count = (bit_count + 7U) & ~std::size_t{7};
  Bytes bits(bit_count / 8U, 0);
  const auto hash_count = static_cast<std::uint8_t>(
      std::clamp<std::size_t>(
          static_cast<std::size_t>(static_cast<double>(bits_per_key) * 0.69),
          1, 30));
  for (auto hash : key_hashes) {
    const auto delta = std::rotl(hash, 15);
    for (std::uint8_t round = 0; round < hash_count; ++round) {
      const auto bit = static_cast<std::size_t>(hash) % bit_count;
      bits[bit / 8U] |= static_cast<Byte>(1U << (bit % 8U));
      hash += delta;
    }
  }
  return BloomFilter(std::move(bits), hash_count);
}

std::size_t BloomFilter::BitsPerKey(std::size_t entries,
                                    double false_positive_rate) {
  if (entries == 0) return 0;
  if (!(false_positive_rate > 0.0 && false_positive_rate < 1.0)) {
    throw Error("Bloom false-positive rate must be between zero and one");
  }
  constexpr double kLn2Squared = 0.4804530139182014;
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(
             std::ceil(-std::log(false_positive_rate) / kLn2Squared)));
}

BloomFilter BloomFilter::Decode(ByteView encoded) {
  if (encoded.empty()) throw Error("empty Bloom filter");
  const auto hash_count = encoded.back();
  if (hash_count == 0 || hash_count > 30) {
    throw Error("invalid Bloom hash count");
  }
  return BloomFilter(Bytes(encoded.begin(), encoded.end() - 1), hash_count);
}

bool BloomFilter::MayContain(std::uint32_t key_hash) const noexcept {
  if (bits_.empty()) return false;
  const auto bit_count = bits_.size() * 8U;
  const auto delta = std::rotl(key_hash, 15);
  for (std::uint8_t round = 0; round < hash_count_; ++round) {
    const auto bit = static_cast<std::size_t>(key_hash) % bit_count;
    if ((bits_[bit / 8U] & static_cast<Byte>(1U << (bit % 8U))) == 0) {
      return false;
    }
    key_hash += delta;
  }
  return true;
}

Bytes BloomFilter::Encode() const {
  Bytes encoded = bits_;
  encoded.push_back(hash_count_);
  return encoded;
}

BlockCache::BlockCache(std::size_t capacity) : capacity_(capacity) {
  if (capacity == 0) throw Error("block cache capacity must be positive");
}

std::size_t BlockCache::KeyHash::operator()(const Key& key) const noexcept {
  const auto first = std::hash<std::uint64_t>{}(key.table_id);
  const auto second = std::hash<std::size_t>{}(key.block_index);
  return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

std::shared_ptr<const Block> BlockCache::Get(std::uint64_t table_id,
                                              std::size_t block_index) {
  std::lock_guard lock(mutex_);
  const Key key{table_id, block_index};
  const auto found = index_.find(key);
  if (found == index_.end()) return {};
  entries_.splice(entries_.begin(), entries_, found->second);
  return found->second->second;
}

void BlockCache::Insert(std::uint64_t table_id, std::size_t block_index,
                        std::shared_ptr<const Block> block) {
  if (!block) throw Error("cannot cache a null block");
  std::lock_guard lock(mutex_);
  const Key key{table_id, block_index};
  if (const auto found = index_.find(key); found != index_.end()) {
    found->second->second = std::move(block);
    entries_.splice(entries_.begin(), entries_, found->second);
    return;
  }
  entries_.emplace_front(key, std::move(block));
  index_[key] = entries_.begin();
  if (entries_.size() > capacity_) {
    index_.erase(entries_.back().first);
    entries_.pop_back();
  }
}

Table::Table(Bytes bytes, std::vector<BlockMeta> meta,
             std::uint32_t meta_offset, BloomFilter bloom)
    : bytes_(std::move(bytes)),
      meta_(std::move(meta)),
      meta_offset_(meta_offset),
      bloom_(std::move(bloom)) {}

std::shared_ptr<Table> Table::open(Bytes encoded) {
  if (encoded.size() < 25) throw Error("table is too short");
  const auto bloom_offset = read_u32(encoded, encoded.size() - 4);
  if (bloom_offset < 8 || bloom_offset > encoded.size() - 9) {
    throw Error("invalid Bloom offset");
  }
  const auto expected_bloom_checksum = read_u32(encoded, encoded.size() - 8);
  const ByteView bloom_bytes = ByteView(encoded).subspan(
      bloom_offset, encoded.size() - 8 - bloom_offset);
  if (checksum(bloom_bytes) != expected_bloom_checksum) {
    throw Error("table Bloom checksum mismatch");
  }
  auto bloom = BloomFilter::Decode(bloom_bytes);
  const std::size_t metadata_end = bloom_offset - 8;
  const auto expected_meta_checksum = read_u32(encoded, metadata_end);
  const auto meta_offset = read_u32(encoded, metadata_end + 4);
  if (meta_offset > metadata_end) throw Error("invalid metadata offset");
  const ByteView meta_bytes =
      ByteView(encoded).subspan(meta_offset, metadata_end - meta_offset);
  if (checksum(meta_bytes) != expected_meta_checksum) {
    throw Error("table metadata checksum mismatch");
  }

  std::size_t cursor = meta_offset;
  const auto count = read_u32(encoded, cursor);
  cursor += 4;
  constexpr std::size_t kMinimumMetaSize = 8;
  if (count == 0 ||
      static_cast<std::size_t>(count) >
          (metadata_end - cursor) / kMinimumMetaSize) {
    throw Error("invalid table block count");
  }
  std::vector<BlockMeta> meta;
  meta.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (cursor + 4 > metadata_end) throw Error("truncated table metadata");
    BlockMeta item;
    item.offset = read_u32(encoded, cursor);
    cursor += 4;
    item.first_key = decode_key(encoded, cursor, metadata_end);
    item.last_key = decode_key(encoded, cursor, metadata_end);
    if (item.offset >= meta_offset ||
        (!meta.empty() && item.offset <= meta.back().offset) ||
        item.last_key < item.first_key) {
      throw Error("invalid table block metadata");
    }
    meta.push_back(std::move(item));
  }
  if (cursor != metadata_end || meta.empty() || meta.front().offset != 0) {
    throw Error("invalid table metadata");
  }
  return std::shared_ptr<Table>(
      new Table(std::move(encoded), std::move(meta), meta_offset,
                std::move(bloom)));
}

bool Table::may_contain(ByteView user_key) const noexcept {
  return bloom_.MayContain(checksum(user_key));
}

std::shared_ptr<const Block> Table::read_block(std::size_t index) const {
  if (index >= meta_.size()) throw Error("table block index out of range");
  const std::size_t begin = meta_[index].offset;
  const std::size_t end =
      index + 1 < meta_.size() ? meta_[index + 1].offset : meta_offset_;
  if (end < begin + 4) throw Error("corrupt table block span");
  const ByteView encoded = ByteView(bytes_).subspan(begin, end - begin - 4);
  if (checksum(encoded) != read_u32(bytes_, end - 4)) {
    throw Error("table block checksum mismatch");
  }
  return Block::decode(Bytes(encoded.begin(), encoded.end()));
}

std::shared_ptr<const Block> Table::read_block_cached(
    std::size_t index, std::uint64_t table_id, BlockCache& cache) const {
  if (auto cached = cache.Get(table_id, index)) return cached;
  auto block = read_block(index);
  cache.Insert(table_id, index, block);
  return block;
}

std::size_t Table::find_block(const InternalKey& key) const {
  const auto it = std::lower_bound(
      meta_.begin(), meta_.end(), key,
      [](const BlockMeta& item, const InternalKey& target) {
        return item.last_key < target;
      });
  return it == meta_.end() ? meta_.size() - 1
                           : static_cast<std::size_t>(it - meta_.begin());
}

std::optional<Bytes> Table::get(ByteView user_key) const {
  return get(user_key, kMaxTimestamp);
}

std::optional<Bytes> Table::get(ByteView user_key,
                                std::uint64_t read_timestamp) const {
  if (bytes_less(user_key, meta_.front().first_key.user_key()) ||
      bytes_less(meta_.back().last_key.user_key(), user_key) ||
      !may_contain(user_key)) {
    return std::nullopt;
  }
  const InternalKey target(Bytes(user_key.begin(), user_key.end()),
                           read_timestamp);
  const auto block_index = find_block(target);
  BlockIterator iterator(read_block(block_index));
  iterator.seek(target);
  if (!iterator.valid() ||
      !std::equal(iterator.key().user_key().begin(),
                  iterator.key().user_key().end(), user_key.begin(),
                  user_key.end()))
    return std::nullopt;
  return Bytes(iterator.value().begin(), iterator.value().end());
}

IteratorPtr Table::iter() const {
  std::vector<KeyValue> entries;
  for (std::size_t i = 0; i < meta_.size(); ++i) {
    BlockIterator iterator(read_block(i));
    while (iterator.valid()) {
      entries.emplace_back(iterator.key(),
                           Bytes(iterator.value().begin(), iterator.value().end()));
      iterator.next();
    }
  }
  return std::make_unique<VectorIterator>(std::move(entries));
}

IteratorPtr Table::iter_from(const InternalKey& key) const {
  std::vector<KeyValue> entries;
  const auto first_block = find_block(key);
  for (std::size_t i = first_block; i < meta_.size(); ++i) {
    BlockIterator iterator(read_block(i));
    if (i == first_block) iterator.seek(key);
    while (iterator.valid()) {
      entries.emplace_back(iterator.key(),
                           Bytes(iterator.value().begin(), iterator.value().end()));
      iterator.next();
    }
  }
  return std::make_unique<VectorIterator>(std::move(entries));
}

TableBuilder::TableBuilder(std::size_t block_size)
    : block_size_(block_size), block_(block_size) {}

bool TableBuilder::empty() const noexcept {
  return meta_.empty() && block_.empty();
}

void TableBuilder::add(const InternalKey& key, ByteView value) {
  if (previous_key_ && !(previous_key_.value() < key)) {
    throw Error("table keys must be added in strictly increasing order");
  }
  if (!block_.add(key, value)) {
    finish_block();
    if (!block_.add(key, value)) throw Error("failed to add to empty block");
  }
  if (!first_key_) first_key_ = key;
  last_key_ = key;
  previous_key_ = key;
  key_hashes_.push_back(checksum(key.user_key()));
}

void TableBuilder::finish_block() {
  if (block_.empty()) return;
  const auto offset = narrow_size<std::uint32_t>(data_.size(), "table");
  const Bytes encoded = block_.finish()->encode();
  data_.insert(data_.end(), encoded.begin(), encoded.end());
  put_u32(data_, checksum(encoded));
  meta_.push_back(BlockMeta{offset, *first_key_, *last_key_});
  block_ = BlockBuilder(block_size_);
  first_key_.reset();
  last_key_.reset();
}

Bytes TableBuilder::finish() {
  finish_block();
  if (meta_.empty()) throw Error("cannot finish an empty table");
  const auto meta_offset = narrow_size<std::uint32_t>(data_.size(), "table");
  Bytes metadata;
  put_u32(metadata, narrow_size<std::uint32_t>(meta_.size(), "block count"));
  for (const auto& item : meta_) {
    put_u32(metadata, item.offset);
    encode_key(metadata, item.first_key);
    encode_key(metadata, item.last_key);
  }
  data_.insert(data_.end(), metadata.begin(), metadata.end());
  put_u32(data_, checksum(metadata));
  put_u32(data_, meta_offset);
  const auto bloom_offset = narrow_size<std::uint32_t>(data_.size(), "table");
  auto bloom = BloomFilter::Build(
      key_hashes_, BloomFilter::BitsPerKey(key_hashes_.size()));
  const auto encoded_bloom = bloom.Encode();
  data_.insert(data_.end(), encoded_bloom.begin(), encoded_bloom.end());
  put_u32(data_, checksum(encoded_bloom));
  put_u32(data_, bloom_offset);
  return std::move(data_);
}

}  // namespace hermesdb
