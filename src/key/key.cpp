#include "hermesdb/key.hpp"

#include <algorithm>

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

InternalKey InternalKey::decode(ByteView encoded) {
  if (encoded.size() < 8) throw Error("internal key is missing timestamp");
  const auto user_size = encoded.size() - 8;
  return InternalKey(Bytes(encoded.begin(),
                           encoded.begin() + static_cast<std::ptrdiff_t>(user_size)),
                     read_u64(encoded, user_size));
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
