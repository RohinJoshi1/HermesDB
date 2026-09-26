#include "hermesdb/block.hpp"

#include <algorithm>
#include <cstring>

namespace hermesdb {

Block::Block(Bytes data, std::vector<std::uint16_t> offsets)
    : data_(std::move(data)), offsets_(std::move(offsets)) {}

std::shared_ptr<Block> Block::decode(Bytes encoded) {
  if (encoded.size() < 2) throw Error("block footer is missing");
  const auto count = read_u16(encoded, encoded.size() - 2);
  if (count == 0) throw Error("empty blocks are not valid");
  const std::size_t footer_size = 2 + static_cast<std::size_t>(count) * 2;
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
  return std::shared_ptr<Block>(new Block(std::move(data), std::move(offsets)));
}

Bytes Block::encode() const {
  Bytes encoded = data_;
  for (const auto offset : offsets_) put_u16(encoded, offset);
  put_u16(encoded, narrow_size<std::uint16_t>(offsets_.size(), "entry count"));
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
  std::size_t cursor = offsets_[index];
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (cursor + rest > data.size() || overlap > first_encoded.size()) {
    throw Error("corrupt block key");
  }
  Bytes encoded;
  encoded.insert(encoded.end(), first_encoded.begin(),
                 first_encoded.begin() + static_cast<std::ptrdiff_t>(overlap));
  encoded.insert(encoded.end(),
                 data.begin() + static_cast<std::ptrdiff_t>(cursor),
                 data.begin() + static_cast<std::ptrdiff_t>(cursor + rest));
  return InternalKey::decode(encoded);
}

BlockBuilder::BlockBuilder(std::size_t target_size) : target_size_(target_size) {
  if (target_size_ < 2) throw Error("block size must be at least two bytes");
}

std::size_t BlockBuilder::estimated_size() const noexcept {
  return data_.size() + offsets_.size() * 2 + 2;
}

bool BlockBuilder::add(const InternalKey& key, ByteView value) {
  const Bytes encoded_key = key.encode();
  const std::size_t overlap =
      first_key_.empty() ? 0 : common_prefix(first_key_, encoded_key);
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
  if (first_key_.empty()) first_key_ = encoded_key;
  return true;
}

std::shared_ptr<Block> BlockBuilder::finish() {
  if (offsets_.empty()) throw Error("cannot finish an empty block");
  return std::shared_ptr<Block>(
      new Block(std::move(data_), std::move(offsets_)));
}

BlockIterator::BlockIterator(std::shared_ptr<const Block> block)
    : block_(std::move(block)) {
  if (!block_) throw Error("block iterator requires a block");
  seek_to_first();
}

bool BlockIterator::valid() const noexcept { return valid_; }

const InternalKey& BlockIterator::key() const {
  if (!valid_) throw Error("iterator is invalid");
  if (!key_ready_) materialize_key();
  return key_;
}

InternalKeyView BlockIterator::key_view() const {
  if (!valid_) throw Error("iterator is invalid");
  return view_;
}

void BlockIterator::materialize_key() const {
  Bytes user;
  view_.materialize_user(user);
  key_.assign(user, view_.timestamp());
  key_ready_ = true;
}

ByteView BlockIterator::value() const {
  if (!valid_) throw Error("iterator is invalid");
  return value_;
}

void BlockIterator::next() {
  if (!valid_) return;
  decode_entry(index_ + 1);
}

void BlockIterator::skip_current_user() {
  if (!valid_) return;
  const auto parked = view_;
  decode_entry(index_ + 1);
  while (valid_ && same_user(view_, parked)) decode_entry(index_ + 1);
}

void BlockIterator::seek_to_first() {
  first_key_.clear();
  decode_entry(0);
}

void BlockIterator::seek(const InternalKey& target) {
  if (block_->empty()) {
    valid_ = false;
    return;
  }
  const auto first_encoded = block_->first_encoded_key();
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
  if (lo >= block_->size()) {
    valid_ = false;
    value_ = {};
    return;
  }
  first_key_ = block_->first_encoded_key();
  decode_entry(lo);
}

void BlockIterator::decode_entry(std::size_t index) {
  if (index >= block_->size()) {
    valid_ = false;
    value_ = {};
    return;
  }
  const ByteView data = block_->data();
  std::size_t cursor = block_->offset(index);
  const auto overlap = read_u16(data, cursor);
  cursor += 2;
  const auto rest = read_u16(data, cursor);
  cursor += 2;
  if (cursor + rest > data.size() || overlap > first_key_.size()) {
    throw Error("corrupt block key");
  }
  const ByteView suffix =
      data.subspan(cursor, static_cast<std::size_t>(rest));
  cursor += rest;
  const auto value_size = read_u16(data, cursor);
  cursor += 2;
  if (cursor + value_size > data.size()) throw Error("corrupt block value");
  if (index == 0) first_key_.assign(suffix.begin(), suffix.end());
  const std::size_t encoded_size = overlap + suffix.size();
  if (encoded_size < 8) throw Error("corrupt block key");
  const std::size_t user_size = encoded_size - 8;
  const ByteView first = first_key_;
  const std::size_t prefix_user = std::min(static_cast<std::size_t>(overlap),
                                           user_size);
  const ByteView prefix = first.subspan(0, prefix_user);
  const ByteView user_suffix =
      overlap >= user_size ? ByteView{}
                           : suffix.subspan(0, user_size - overlap);
  std::uint64_t timestamp = 0;
  if (suffix.size() >= 8) {
    timestamp = read_u64(suffix, suffix.size() - 8);
  } else {
    Byte stamp[8];
    const std::size_t from_first = 8 - suffix.size();
    std::memcpy(stamp, first.data() + (overlap - from_first), from_first);
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
