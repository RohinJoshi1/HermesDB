#pragma once

#include "tiny_lsm/common.hpp"

#include <compare>
#include <limits>

namespace tiny_lsm {

inline constexpr std::uint64_t kMaxTimestamp =
    std::numeric_limits<std::uint64_t>::max();

class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(std::string_view user_key, std::uint64_t timestamp);
  InternalKey(Bytes user_key, std::uint64_t timestamp);

  [[nodiscard]] ByteView user_key() const noexcept { return user_key_; }
  [[nodiscard]] std::uint64_t timestamp() const noexcept { return timestamp_; }
  [[nodiscard]] bool empty() const noexcept { return user_key_.empty(); }
  [[nodiscard]] Bytes encode() const;
  [[nodiscard]] static InternalKey decode(ByteView encoded);

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

}  // namespace tiny_lsm
