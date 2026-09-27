#include "hermesdb/block.hpp"

#include <algorithm>
#include <cstring>

namespace hermesdb {

Block::Block(Bytes data, std::vector<std::uint16_t> offsets,
             std::uint16_t restart_interval)
    : data_(std::move(data)),
      offsets_(std::move(offsets)),
      restart_interval_(restart_interval) {}

std::size_t Block::restart_base(std::size_t index) const noexcept {
  if (restart_interval_ <= 1) return 0;
  return (index / restart_interval_) * restart_interval_;
}

std::shared_ptr<Block> Block::decode(Bytes encoded) {
  if (encoded.size() < 2) throw Error("block footer is missing");
  std::uint16_t count = 0;
  std::uint16_t interval = 0;
  std::size_t footer_size = 0;
  if (encoded.size() >= 8 &&
      read_u32(encoded, encoded.size() - 4) == kBlockRestartMagic) {
    interval = read_u16(encoded, encoded.size() - 6);
    count = read_u16(encoded, encoded.size() - 8);
    footer_size = 8 + static_cast<std::size_t>(count) * 2;
    if (interval == 0) throw Error("invalid block restart interval");
  } else {
    count = read_u16(encoded, encoded.size() - 2);
    footer_size = 2 + static_cast<std::size_t>(count) * 2;
  }
  if (count == 0) throw Error("empty blocks are not valid");
  if (footer_size > encoded.size()) throw Error("invalid block footer");
  const std::size_t data_size = encoded.size() - footer_size;
  std::vector<std::uint16_t> offsets;
  offsets.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto offset = read_u16(encoded, data_size + i * 2);
    if (offset >= data_size || (!offsets.empty() && offset <= offsets.back())) {
      throw Error("invalid block entry offset");
    }
    offsets.push_back(offset);
  }
  if (offsets.front() != 0) throw Error("first block entry must start at zero");
  Bytes data(encoded.begin(), encoded.begin() +
                                  static_cast<std::ptrdiff_t>(data_size));
  return std::shared_ptr<Block>(
      new Block(std::move(data), std::move(offsets), interval));
}

Bytes Block::encode() const {
  Bytes encoded = data_;
  for (const auto offset : offsets_) put_u16(encoded, offset);
  put_u16(encoded, narrow_size<std::uint16_t>(offsets_.size(), "entry count"));
  if (restart_interval_ != 0) {
    put_u16(encoded, restart_interval_);
    put_u32(encoded, kBlockRestartMagic);
  }
  return encoded;
}

std::uint16_t Block::offset(std::size_t index) const {
  if (index >= offsets_.size()) throw Error("block entry index out of range");
  return offsets_[index];
}

Bytes Block::first_encoded_key() const {
  if (offsets_.empty()) throw Error("empty block has no first key");
  const ByteView data = data_;
  std::size_t cursor = offsets_.front();
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (overlap != 0 || cursor + rest > data.size()) {
    throw Error("corrupt block first key");
  }
  return Bytes(data.begin() + static_cast<std::ptrdiff_t>(cursor),
               data.begin() + static_cast<std::ptrdiff_t>(cursor + rest));
}

InternalKey Block::key_at(std::size_t index,
                            const Bytes& first_encoded) const {
  if (index >= offsets_.size()) throw Error("block entry index out of range");
  const ByteView data = data_;
  const auto base = restart_base(index);
  ByteView restart = first_encoded;
  Bytes restart_owned;
  if (base != 0) {
    std::size_t cursor = offsets_[base];
    const auto overlap = read_u16(data, cursor);
    cursor += 2;
    const auto rest = read_u16(data, cursor);
    cursor += 2;
    if (overlap != 0 || cursor + rest > data.size()) {
      throw Error("corrupt block restart key");
    }
    restart_owned.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                         data.begin() +
                             static_cast<std::ptrdiff_t>(cursor + rest));
    restart = restart_owned;
  }
  std::size_t cursor = offsets_[index];
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (cursor + rest > data.size() || overlap > restart.size()) {
    throw Error("corrupt block key");
  }
  InternalKey key;
  key.decode_prefixed(restart, overlap,
                      data.subspan(cursor, static_cast<std::size_t>(rest)));
  return key;
}

BlockBuilder::BlockBuilder(std::size_t target_size) : target_size_(target_size) {
  if (target_size_ < 2) throw Error("block size must be at least two bytes");
}

std::size_t BlockBuilder::estimated_size() const noexcept {
  return data_.size() + offsets_.size() * 2 + 8;
}

bool BlockBuilder::add(const InternalKey& key, ByteView value) {
  const Bytes encoded_key = key.encode();
  const bool restart =
      offsets_.empty() ||
      offsets_.size() % kBlockRestartInterval == 0;
  const std::size_t overlap =
      restart ? 0 : common_prefix(restart_key_, encoded_key);
  const std::size_t rest = encoded_key.size() - overlap;
  const std::size_t entry_size = 2 + 2 + rest + 2 + value.size();
  if (!offsets_.empty() &&
      estimated_size() + entry_size + 2 > target_size_) {
    return false;
  }
  if (data_.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw Error("block data exceeds 16-bit offset range");
  }
  const auto overlap16 = narrow_size<std::uint16_t>(overlap, "key overlap");
  const auto rest16 = narrow_size<std::uint16_t>(rest, "key suffix");
  const auto value16 = narrow_size<std::uint16_t>(value.size(), "value");
  offsets_.push_back(static_cast<std::uint16_t>(data_.size()));
  put_u16(data_, overlap16);
  put_u16(data_, rest16);
  data_.insert(data_.end(),
               encoded_key.begin() + static_cast<std::ptrdiff_t>(overlap),
               encoded_key.end());
  put_u16(data_, value16);
  data_.insert(data_.end(), value.begin(), value.end());
  if (restart) restart_key_ = encoded_key;
  return true;
}

std::shared_ptr<Block> BlockBuilder::finish() {
  if (offsets_.empty()) throw Error("cannot finish an empty block");
  return std::shared_ptr<Block>(new Block(std::move(data_), std::move(offsets_),
                                          kBlockRestartInterval));
}

BlockIterator::BlockIterator(std::shared_ptr<const Block> block)
    : block_(std::move(block)) {
  if (!block_) throw Error("block iterator requires a block");
  seek_to_first();
}

bool BlockIterator::valid() const noexcept {
  return !batch_.empty() || index_ < block_->size();
}

const InternalKey& BlockIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  if (!key_ready_) materialize_key();
  return key_;
}

InternalKeyView BlockIterator::key_view() const {
  if (!valid()) throw Error("iterator is invalid");
  ensure_batch();
  return batch_.current().key;
}

void BlockIterator::materialize_key() const {
  Bytes user;
  key_view().materialize_user(user);
  key_.assign(user, key_view().timestamp());
  key_ready_ = true;
}

ByteView BlockIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  ensure_batch();
  return batch_.current().value;
}

void BlockIterator::next() {
  if (!valid()) return;
  ensure_batch();
  if (batch_.empty()) return;
  batch_.pop();
  key_ready_ = false;
  if (batch_.empty() && index_ >= block_->size()) valid_ = false;
}

void BlockIterator::skip_current_user() {
  if (!valid()) return;
  Bytes parked;
  key_view().materialize_user(parked);
  next();
  while (valid() && same_user(key_view(), parked)) next();
}

std::size_t BlockIterator::pull(std::span<ScanRow> out) {
  ensure_batch();
  std::size_t n = 0;
  while (n < out.size() && !batch_.empty()) {
    out[n++] = batch_.current();
    batch_.pop();
    key_ready_ = false;
  }
  if (batch_.empty() && index_ >= block_->size()) valid_ = false;
  return n;
}

void BlockIterator::ensure_batch() const {
  if (!batch_.empty()) return;
  refill();
}

void BlockIterator::refill() const {
  batch_.reset();
  if (index_ >= block_->size()) {
    valid_ = false;
    return;
  }
  load_restart(index_);
  const auto base = restart_index_;
  while (batch_.size < kScanBatch && index_ < block_->size() &&
         block_->restart_base(index_) == base) {
    decode_entry(index_);
    batch_.rows[batch_.size++] = {view_, value_};
    ++index_;
  }
  valid_ = batch_.size != 0;
}

void BlockIterator::seek_to_first() {
  restart_index_ = std::numeric_limits<std::size_t>::max();
  restart_key_.clear();
  index_ = 0;
  batch_.reset();
  refill();
}

void BlockIterator::seek(const InternalKey& target) {
  if (block_->empty()) {
    valid_ = false;
    return;
  }
  const auto first_encoded = block_->first_encoded_key();
  const auto interval = block_->restart_interval();
  std::size_t start = 0;
  if (interval > 1) {
    const std::size_t restarts =
        (block_->size() + interval - 1) / interval;
    std::size_t lo = 0;
    std::size_t hi = restarts;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (block_->key_at(mid * interval, first_encoded) < target) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    start = lo == 0 ? 0 : (lo - 1) * interval;
  } else {
    std::size_t lo = 0;
    std::size_t hi = block_->size();
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (block_->key_at(mid, first_encoded) < target) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    start = lo;
  }
  restart_index_ = std::numeric_limits<std::size_t>::max();
  batch_.reset();
  if (start >= block_->size()) {
    index_ = start;
    valid_ = false;
    value_ = {};
    return;
  }
  index_ = start;
  refill();
  if (interval > 1) {
    const auto want = as_view(target);
    while (valid() && compare_internal(key_view(), want) < 0) next();
  }
}

void BlockIterator::load_restart(std::size_t index) const {
  const auto base = block_->restart_base(index);
  if (base == restart_index_) return;
  const ByteView data = block_->data();
  std::size_t cursor = block_->offset(base);
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (overlap != 0 || cursor + rest > data.size()) {
    throw Error("corrupt block restart key");
  }
  restart_key_.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                      data.begin() +
                          static_cast<std::ptrdiff_t>(cursor + rest));
  restart_index_ = base;
}

void BlockIterator::decode_entry(std::size_t index) const {
  if (index >= block_->size()) {
    valid_ = false;
    value_ = {};
    return;
  }
  load_restart(index);
  const ByteView data = block_->data();
  std::size_t cursor = block_->offset(index);
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (cursor + rest > data.size() || overlap > restart_key_.size()) {
    throw Error("corrupt block key");
  }
  const ByteView suffix =
      data.subspan(cursor, static_cast<std::size_t>(rest));
  cursor += rest;
  const auto value_size = read_u16(data, cursor);
  cursor += 2;
  if (cursor + value_size > data.size()) throw Error("corrupt block value");
  const std::size_t encoded_size = overlap + suffix.size();
  if (encoded_size < 8) throw Error("corrupt block key");
  const std::size_t user_size = encoded_size - 8;
  const ByteView restart = restart_key_;
  const std::size_t prefix_user = std::min(static_cast<std::size_t>(overlap),
                                           user_size);
  const ByteView prefix = restart.subspan(0, prefix_user);
  const ByteView user_suffix =
      overlap >= user_size ? ByteView{}
                           : suffix.subspan(0, user_size - overlap);
  std::uint64_t timestamp = 0;
  if (suffix.size() >= 8) {
    timestamp = read_u64(suffix, suffix.size() - 8);
  } else {
    Byte stamp[8];
    const std::size_t from_first = 8 - suffix.size();
    std::memcpy(stamp, restart.data() + (overlap - from_first), from_first);
    if (!suffix.empty()) {
      std::memcpy(stamp + from_first, suffix.data(), suffix.size());
    }
    timestamp = read_u64(ByteView{stamp, 8}, 0);
  }
  view_ = InternalKeyView(prefix, user_suffix, timestamp);
  key_ready_ = false;
  value_ = data.subspan(cursor, value_size);
  index_ = index;
  valid_ = true;
}

}  // namespace hermesdb
