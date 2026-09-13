#include "tiny_lsm/block.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace tiny_lsm {
namespace {

constexpr std::size_t kU16 = sizeof(std::uint16_t);

}  // namespace

Block::Block(Bytes data, std::vector<std::uint16_t> offsets)
    : data_(std::move(data)), offsets_(std::move(offsets)) {}

std::shared_ptr<Block> Block::decode(Bytes encoded) {
  if (encoded.size() < kU16) {
    throw Error("encoded block is missing entry count");
  }
  const auto entry_count = read_u16(encoded, encoded.size() - kU16);
  const std::size_t footer_size =
      static_cast<std::size_t>(entry_count) * kU16 + kU16;
  if (footer_size > encoded.size()) {
    throw Error("block footer exceeds encoded block size");
  }
  const std::size_t data_size = encoded.size() - footer_size;
  if (entry_count == 0 && data_size != 0) {
    throw Error("empty block contains unindexed data");
  }

  std::vector<std::uint16_t> offsets;
  offsets.reserve(entry_count);
  for (std::size_t index = 0; index < entry_count; ++index) {
    const auto offset = read_u16(encoded, data_size + index * kU16);
    if (static_cast<std::size_t>(offset) >= data_size) {
      throw Error("block entry offset is outside data section");
    }
    if (index == 0 && offset != 0) {
      throw Error("first block entry must begin at offset zero");
    }
    if (!offsets.empty() && offset <= offsets.back()) {
      throw Error("block entry offsets must be strictly increasing");
    }
    offsets.push_back(offset);
  }

  Bytes first_key;
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const std::size_t entry_start = offsets[index];
    const std::size_t entry_end =
        index + 1 < offsets.size() ? offsets[index + 1] : data_size;
    if (entry_end - entry_start < 3 * kU16) {
      throw Error("block entry is too short");
    }
    const auto overlap = read_u16(encoded, entry_start);
    const auto rest = read_u16(encoded, entry_start + kU16);
    if (index == 0) {
      if (overlap != 0 || rest == 0) {
        throw Error("first block key must be stored in full");
      }
    } else if (overlap > first_key.size()) {
      throw Error("block key overlap exceeds first key");
    }
    const std::size_t suffix_start = entry_start + 2 * kU16;
    const std::size_t suffix_end = suffix_start + rest;
    if (suffix_end + kU16 > entry_end) {
      throw Error("block key suffix extends beyond entry boundary");
    }
    Bytes reconstructed;
    reconstructed.insert(reconstructed.end(), first_key.begin(),
                         first_key.begin() + static_cast<std::ptrdiff_t>(overlap));
    reconstructed.insert(
        reconstructed.end(),
        encoded.begin() + static_cast<std::ptrdiff_t>(suffix_start),
        encoded.begin() + static_cast<std::ptrdiff_t>(suffix_end));
    if (reconstructed.empty()) {
      throw Error("block entry contains an empty key");
    }
    if (index == 0) first_key = reconstructed;
    const auto value_length = read_u16(encoded, suffix_end);
    const std::size_t value_end = suffix_end + kU16 + value_length;
    if (value_end != entry_end) {
      throw Error("block value does not match entry boundary");
    }
  }

  encoded.resize(data_size);
  return std::shared_ptr<Block>(
      new Block(std::move(encoded), std::move(offsets)));
}

Bytes Block::encode() const {
  Bytes encoded = data_;
  encoded.reserve(data_.size() + offsets_.size() * 2 + 2);
  for (auto offset : offsets_) {
    put_u16(encoded, offset);
  }
  put_u16(encoded,
          narrow_size<std::uint16_t>(offsets_.size(), "block entry count"));
  return encoded;
}

std::uint16_t Block::offset(std::size_t index) const {
  if (index >= offsets_.size()) {
    throw Error("block offset index out of range");
  }
  return offsets_[index];
}

BlockBuilder::BlockBuilder(std::size_t target_size)
    : target_size_(target_size) {}

bool BlockBuilder::add(ByteView key, ByteView value) {
  if (key.empty()) {
    throw Error("block key cannot be empty");
  }

  const std::size_t overlap =
      first_key_.empty() ? 0 : common_prefix(first_key_, key);
  const std::size_t rest = key.size() - overlap;
  const std::size_t entry_size = 2 * kU16 + rest + kU16 + value.size();
  const std::size_t prospective_size =
      estimated_size() + entry_size + kU16;
  if (!offsets_.empty() && prospective_size > target_size_) {
    return false;
  }

  const auto entry_offset =
      narrow_size<std::uint16_t>(data_.size(), "block entry offset");
  static_cast<void>(
      narrow_size<std::uint16_t>(offsets_.size() + 1, "block entry count"));
  const auto overlap16 = narrow_size<std::uint16_t>(overlap, "key overlap");
  const auto rest16 = narrow_size<std::uint16_t>(rest, "key suffix");
  const auto value16 = narrow_size<std::uint16_t>(value.size(), "block value");

  offsets_.push_back(entry_offset);
  put_u16(data_, overlap16);
  put_u16(data_, rest16);
  data_.insert(data_.end(), key.begin() + static_cast<std::ptrdiff_t>(overlap),
               key.end());
  put_u16(data_, value16);
  data_.insert(data_.end(), value.begin(), value.end());
  if (first_key_.empty()) {
    first_key_.assign(key.begin(), key.end());
  }
  return true;
}

std::size_t BlockBuilder::estimated_size() const noexcept {
  return data_.size() + offsets_.size() * 2 + 2;
}

std::shared_ptr<Block> BlockBuilder::finish() {
  if (empty()) {
    throw Error("cannot finish an empty block");
  }
  return std::shared_ptr<Block>(
      new Block(std::move(data_), std::move(offsets_)));
}

BlockIterator::BlockIterator(std::shared_ptr<const Block> block)
    : block_(std::move(block)) {
  seek_to_first();
}

bool BlockIterator::valid() const noexcept { return valid_; }

ByteView BlockIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return key_;
}

ByteView BlockIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return value_;
}

void BlockIterator::next() {
  if (!valid()) return;
  decode_entry(index_ + 1);
}

void BlockIterator::seek_to_first() { decode_entry(0); }

void BlockIterator::seek(ByteView target) {
  if (block_ == nullptr || block_->empty()) {
    decode_entry(0);
    return;
  }
  std::size_t left = 0;
  std::size_t right = block_->size();
  while (left < right) {
    const std::size_t mid = left + (right - left) / 2;
    decode_entry(mid);
    if (bytes_less(key_, target)) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  decode_entry(left);
}

void BlockIterator::decode_entry(std::size_t index) {
  if (block_ == nullptr || index >= block_->size()) {
    key_.clear();
    value_ = {};
    valid_ = false;
    return;
  }

  const ByteView data = block_->data();
  if (first_key_.empty()) {
    const auto overlap = read_u16(data, 0);
    const auto rest = read_u16(data, kU16);
    if (overlap != 0) throw Error("first block key must have zero overlap");
    first_key_.assign(data.begin() + static_cast<std::ptrdiff_t>(2 * kU16),
                      data.begin() + static_cast<std::ptrdiff_t>(2 * kU16 + rest));
  }

  std::size_t cursor = block_->offset(index);
  const auto overlap = read_u16(data, cursor);
  cursor += kU16;
  const auto rest = read_u16(data, cursor);
  cursor += kU16;
  if (overlap > first_key_.size() || cursor + rest > data.size()) {
    throw Error("corrupt block key");
  }
  key_.assign(first_key_.begin(),
              first_key_.begin() + static_cast<std::ptrdiff_t>(overlap));
  key_.insert(key_.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
              data.begin() + static_cast<std::ptrdiff_t>(cursor + rest));
  cursor += rest;
  const auto value_length = read_u16(data, cursor);
  cursor += kU16;
  if (cursor + value_length > data.size()) {
    throw Error("corrupt block value");
  }
  value_ = data.subspan(cursor, value_length);
  index_ = index;
  valid_ = true;
}

}  // namespace tiny_lsm
