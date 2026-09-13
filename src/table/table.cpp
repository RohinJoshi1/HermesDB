#include "tiny_lsm/table.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace tiny_lsm {
namespace {

void encode_key(Bytes& out, ByteView key) {
  put_u16(out, narrow_size<std::uint16_t>(key.size(), "metadata key"));
  out.insert(out.end(), key.begin(), key.end());
}

Bytes decode_key(ByteView bytes, std::size_t& cursor, std::size_t limit) {
  const auto size = read_u16(bytes, cursor);
  cursor += 2;
  if (cursor + size > limit) throw Error("truncated table metadata key");
  Bytes key(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor + size));
  cursor += size;
  return key;
}

}  // namespace

BloomFilter BloomFilter::Build(std::span<const std::uint32_t> key_hashes,
                               std::size_t bits_per_key) {
  std::size_t bit_count =
      std::max<std::size_t>(64, key_hashes.size() * bits_per_key);
  bit_count = (bit_count + 7U) & ~std::size_t{7};
  Bytes bits(bit_count / 8U, 0);
  const auto hash_count = static_cast<std::uint8_t>(std::clamp<std::size_t>(
      static_cast<std::size_t>(static_cast<double>(bits_per_key) * 0.69), 1,
      30));
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
        bytes_less(item.last_key, item.first_key)) {
      throw Error("invalid table block metadata");
    }
    meta.push_back(std::move(item));
  }
  if (cursor != metadata_end || meta.empty() || meta.front().offset != 0) {
    throw Error("invalid table metadata");
  }
  return std::shared_ptr<Table>(new Table(std::move(encoded), std::move(meta),
                                          meta_offset, std::move(bloom)));
}

ByteView Table::first_key() const noexcept { return meta_.front().first_key; }

ByteView Table::last_key() const noexcept { return meta_.back().last_key; }

bool Table::may_contain(ByteView user_key) const noexcept {
  return bloom_.MayContain(checksum(user_key));
}

std::optional<Bytes> Table::get(ByteView key) const {
  auto iterator = iter_from(key);
  if (!iterator->valid() || !bytes_equal(iterator->key(), key)) {
    return std::nullopt;
  }
  return Bytes(iterator->value().begin(), iterator->value().end());
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

std::size_t Table::find_block(ByteView key) const {
  const auto it = std::lower_bound(
      meta_.begin(), meta_.end(), key,
      [](const BlockMeta& item, ByteView target) {
        return bytes_less(item.last_key, target);
      });
  return it == meta_.end() ? meta_.size() - 1
                           : static_cast<std::size_t>(it - meta_.begin());
}

IteratorPtr Table::iter() const {
  auto iterator = std::make_unique<TableIterator>(shared_from_this());
  iterator->seek_to_first();
  return iterator;
}

IteratorPtr Table::iter_from(ByteView key) const {
  auto iterator = std::make_unique<TableIterator>(shared_from_this());
  iterator->seek(key);
  return iterator;
}

TableBuilder::TableBuilder(std::size_t block_size)
    : block_size_(block_size), block_(block_size) {}

bool TableBuilder::empty() const noexcept {
  return meta_.empty() && block_.empty();
}

void TableBuilder::add(ByteView key, ByteView value) {
  if (previous_key_ && !bytes_less(*previous_key_, key)) {
    throw Error("table keys must be added in strictly increasing order");
  }
  if (!block_.add(key, value)) {
    finish_block();
    if (!block_.add(key, value)) throw Error("failed to add to empty block");
  }
  if (!first_key_) first_key_ = Bytes(key.begin(), key.end());
  last_key_ = Bytes(key.begin(), key.end());
  previous_key_ = last_key_;
  key_hashes_.push_back(checksum(key));
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

TableIterator::TableIterator(std::shared_ptr<const Table> table)
    : table_(std::move(table)) {}

bool TableIterator::valid() const noexcept {
  return block_iter_ && block_iter_->valid();
}

ByteView TableIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return block_iter_->key();
}

ByteView TableIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return block_iter_->value();
}

void TableIterator::next() {
  if (!valid()) return;
  block_iter_->next();
  if (!block_iter_->valid()) {
    open_block(block_index_ + 1, nullptr);
  }
}

void TableIterator::seek_to_first() { open_block(0, nullptr); }

void TableIterator::seek(ByteView target) {
  if (!table_ || table_->num_blocks() == 0) {
    block_iter_.reset();
    return;
  }
  const auto index = table_->find_block(target);
  open_block(index, &target);
  if (!valid() && index + 1 < table_->num_blocks()) {
    open_block(index + 1, nullptr);
  }
}

void TableIterator::open_block(std::size_t index, const ByteView* target) {
  if (!table_ || index >= table_->num_blocks()) {
    block_iter_.reset();
    return;
  }
  block_index_ = index;
  block_iter_ = std::make_unique<BlockIterator>(table_->read_block(index));
  if (target != nullptr) {
    block_iter_->seek(*target);
  }
}

ConcatIterator::ConcatIterator(std::vector<std::shared_ptr<const Table>> tables)
    : tables_(std::move(tables)) {}

bool ConcatIterator::valid() const noexcept {
  return child_ && child_->valid();
}

ByteView ConcatIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->key();
}

ByteView ConcatIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->value();
}

void ConcatIterator::next() {
  if (!valid()) return;
  child_->next();
  if (!child_->valid()) {
    open_table(index_ + 1, nullptr);
  }
}

void ConcatIterator::seek_to_first() { open_table(0, nullptr); }

void ConcatIterator::seek(ByteView target) {
  if (tables_.empty()) {
    child_.reset();
    return;
  }
  const auto it = std::lower_bound(
      tables_.begin(), tables_.end(), target,
      [](const std::shared_ptr<const Table>& table, ByteView wanted) {
        return bytes_less(table->last_key(), wanted);
      });
  if (it == tables_.end()) {
    child_.reset();
    return;
  }
  open_table(static_cast<std::size_t>(it - tables_.begin()), &target);
}

void ConcatIterator::open_table(std::size_t index, const ByteView* target) {
  if (index >= tables_.size() || !tables_[index]) {
    child_.reset();
    return;
  }
  index_ = index;
  child_ = std::make_unique<TableIterator>(tables_[index]);
  if (target != nullptr) {
    child_->seek(*target);
  } else {
    child_->seek_to_first();
  }
  if (!child_->valid() && index + 1 < tables_.size()) {
    open_table(index + 1, nullptr);
  }
}

}  // namespace tiny_lsm
