#include "hermesdb/key.hpp"

#include <algorithm>
#include <cstring>

namespace hermesdb {

InternalKey::InternalKey(std::string_view user_key, std::uint64_t timestamp)
    : user_key_(as_bytes(user_key).begin(), as_bytes(user_key).end()),
      timestamp_(timestamp) {}

InternalKey::InternalKey(Bytes user_key, std::uint64_t timestamp)
    : user_key_(std::move(user_key)), timestamp_(timestamp) {}

Bytes InternalKey::encode() const {
  Bytes encoded = user_key_;
  put_u64(encoded, timestamp_);
  return encoded;
}

void InternalKey::assign(ByteView user_key, std::uint64_t timestamp) {
  user_key_.assign(user_key.begin(), user_key.end());
  timestamp_ = timestamp;
}

void InternalKey::assign(const InternalKey& other) {
  if (this == &other) return;
  assign(other.user_key(), other.timestamp());
}

void InternalKey::decode_from(ByteView encoded) {
  if (encoded.size() < 8) throw Error("internal key is missing timestamp");
  const auto user_size = encoded.size() - 8;
  assign(encoded.first(user_size), read_u64(encoded, user_size));
}

void InternalKey::decode_prefixed(ByteView first_encoded, std::size_t overlap,
                                  ByteView rest) {
  const std::size_t encoded_size = overlap + rest.size();
  if (encoded_size < 8 || overlap > first_encoded.size()) {
    throw Error("internal key is missing timestamp");
  }
  const std::size_t user_size = encoded_size - 8;
  user_key_.resize(user_size);
  if (overlap >= user_size) {
    std::memcpy(user_key_.data(), first_encoded.data(), user_size);
  } else {
    if (overlap != 0) {
      std::memcpy(user_key_.data(), first_encoded.data(), overlap);
    }
    std::memcpy(user_key_.data() + overlap, rest.data(), user_size - overlap);
  }
  if (rest.size() >= 8) {
    timestamp_ = read_u64(rest, rest.size() - 8);
    return;
  }
  Byte stamp[8];
  const std::size_t from_first = 8 - rest.size();
  std::memcpy(stamp, first_encoded.data() + (overlap - from_first), from_first);
  if (!rest.empty()) {
    std::memcpy(stamp + from_first, rest.data(), rest.size());
  }
  timestamp_ = read_u64(ByteView{stamp, 8}, 0);
}

InternalKey InternalKey::decode(ByteView encoded) {
  InternalKey key;
  key.decode_from(encoded);
  return key;
}

std::strong_ordering operator<=>(const InternalKey& lhs,
                                 const InternalKey& rhs) noexcept {
  const auto key_order = std::lexicographical_compare_three_way(
      lhs.user_key_.begin(), lhs.user_key_.end(), rhs.user_key_.begin(),
      rhs.user_key_.end());
  if (key_order != 0) return key_order;
  return rhs.timestamp_ <=> lhs.timestamp_;
}

}  // namespace hermesdb
