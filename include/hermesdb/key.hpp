#pragma once

#include "hermesdb/common.hpp"

#include <compare>
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

}  // namespace hermesdb
