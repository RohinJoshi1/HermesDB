#pragma once

#include "hermesdb/common.hpp"

#include <algorithm>
#include <compare>
#include <cstring>
#include <limits>

namespace hermesdb {

inline constexpr std::uint64_t kMaxTimestamp =
    std::numeric_limits<std::uint64_t>::max();

class InternalKey {
 public:
  InternalKey() = default;
  explicit InternalKey(std::string_view user_key, std::uint64_t timestamp = 0);
  explicit InternalKey(Bytes user_key, std::uint64_t timestamp = 0);

  [[nodiscard]] ByteView user_key() const noexcept { return user_key_; }
  [[nodiscard]] std::uint64_t timestamp() const noexcept { return timestamp_; }
  [[nodiscard]] bool empty() const noexcept { return user_key_.empty(); }
  [[nodiscard]] Bytes encode() const;
  [[nodiscard]] static InternalKey decode(ByteView encoded);
  void assign(ByteView user_key, std::uint64_t timestamp);
  void assign(const InternalKey& other);
  void decode_from(ByteView encoded);
  void decode_prefixed(ByteView first_encoded, std::size_t overlap,
                       ByteView rest);

  friend bool operator==(const InternalKey&, const InternalKey&) = default;
  friend std::strong_ordering operator<=>(const InternalKey& lhs,
                                           const InternalKey& rhs) noexcept;

 private:
  Bytes user_key_;
  std::uint64_t timestamp_{};
};

struct InternalKeyLess {
  [[nodiscard]] bool operator()(const InternalKey& lhs,
                                const InternalKey& rhs) const noexcept {
    return lhs < rhs;
  }
};

// Prefix + suffix of a prefix-compressed user key. The timestamp is already
// parsed. Callers must not assume the user key is contiguous.
class InternalKeyView {
 public:
  InternalKeyView() = default;
  InternalKeyView(ByteView user_key, std::uint64_t timestamp) noexcept
      : prefix_(user_key), timestamp_(timestamp) {}
  InternalKeyView(ByteView prefix, ByteView suffix,
                  std::uint64_t timestamp) noexcept
      : prefix_(prefix), suffix_(suffix), timestamp_(timestamp) {}

  [[nodiscard]] ByteView prefix() const noexcept { return prefix_; }
  [[nodiscard]] ByteView suffix() const noexcept { return suffix_; }
  [[nodiscard]] std::size_t user_size() const noexcept {
    return prefix_.size() + suffix_.size();
  }
  [[nodiscard]] bool empty() const noexcept { return user_size() == 0; }
  [[nodiscard]] std::uint64_t timestamp() const noexcept { return timestamp_; }

  void materialize_user(Bytes& out) const {
    out.resize(user_size());
    if (!prefix_.empty()) {
      std::memcpy(out.data(), prefix_.data(), prefix_.size());
    }
    if (!suffix_.empty()) {
      std::memcpy(out.data() + prefix_.size(), suffix_.data(), suffix_.size());
    }
  }

 private:
  ByteView prefix_{};
  ByteView suffix_{};
  std::uint64_t timestamp_{};
};

[[nodiscard]] inline std::strong_ordering compare_concat(
    ByteView a0, ByteView a1, ByteView b0, ByteView b1) noexcept {
  const std::size_t as = a0.size() + a1.size();
  const std::size_t bs = b0.size() + b1.size();
  std::size_t ia = 0;
  std::size_t ib = 0;
  while (ia < as && ib < bs) {
    const ByteView sa =
        ia < a0.size() ? a0.subspan(ia) : a1.subspan(ia - a0.size());
    const ByteView sb =
        ib < b0.size() ? b0.subspan(ib) : b1.subspan(ib - b0.size());
    const std::size_t take = std::min(sa.size(), sb.size());
    const int cmp = std::memcmp(sa.data(), sb.data(), take);
    if (cmp != 0) {
      return cmp < 0 ? std::strong_ordering::less
                     : std::strong_ordering::greater;
    }
    ia += take;
    ib += take;
  }
  return as <=> bs;
}

[[nodiscard]] inline std::strong_ordering compare_user(
    const InternalKeyView& lhs, const InternalKeyView& rhs) noexcept {
  return compare_concat(lhs.prefix(), lhs.suffix(), rhs.prefix(), rhs.suffix());
}

[[nodiscard]] inline std::strong_ordering compare_user(
    const InternalKeyView& lhs, ByteView rhs) noexcept {
  return compare_concat(lhs.prefix(), lhs.suffix(), rhs, {});
}

[[nodiscard]] inline std::strong_ordering compare_internal(
    const InternalKeyView& lhs, const InternalKeyView& rhs) noexcept {
  if (const auto order = compare_user(lhs, rhs); order != 0) return order;
  return rhs.timestamp() <=> lhs.timestamp();
}

[[nodiscard]] inline bool same_user(const InternalKeyView& lhs,
                                    const InternalKeyView& rhs) noexcept {
  return compare_user(lhs, rhs) == 0;
}

[[nodiscard]] inline bool same_user(const InternalKeyView& lhs,
                                    ByteView rhs) noexcept {
  return compare_user(lhs, rhs) == 0;
}

[[nodiscard]] inline InternalKeyView as_view(const InternalKey& key) noexcept {
  return InternalKeyView(key.user_key(), key.timestamp());
}

}  // namespace hermesdb
