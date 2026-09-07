#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tiny_lsm {

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ByteView = std::span<const Byte>;

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] ByteView as_bytes(std::string_view value) noexcept;
[[nodiscard]] std::string as_string(ByteView value);

void put_u16(Bytes& out, std::uint16_t value);
void put_u32(Bytes& out, std::uint32_t value);
void put_u64(Bytes& out, std::uint64_t value);
[[nodiscard]] std::uint16_t read_u16(ByteView data, std::size_t offset);
[[nodiscard]] std::uint32_t read_u32(ByteView data, std::size_t offset);
[[nodiscard]] std::uint64_t read_u64(ByteView data, std::size_t offset);
[[nodiscard]] std::size_t common_prefix(ByteView lhs, ByteView rhs) noexcept;
[[nodiscard]] std::uint32_t checksum(ByteView data) noexcept;

template <typename T>
[[nodiscard]] T narrow_size(std::size_t value, std::string_view what) {
  if (value > static_cast<std::size_t>(std::numeric_limits<T>::max())) {
    throw Error(std::string(what) + " is too large");
  }
  return static_cast<T>(value);
}

}  // namespace tiny_lsm
