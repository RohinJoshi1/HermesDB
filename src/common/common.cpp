#include "tiny_lsm/common.hpp"

#include <algorithm>
#include <cstring>

namespace tiny_lsm {

ByteView as_bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const Byte*>(value.data()), value.size()};
}

std::string as_string(ByteView value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool bytes_less(ByteView lhs, ByteView rhs) noexcept {
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                      rhs.end());
}

bool bytes_equal(ByteView lhs, ByteView rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool bytes_less_equal(ByteView lhs, ByteView rhs) noexcept {
  return bytes_less(lhs, rhs) || bytes_equal(lhs, rhs);
}

void put_u16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<Byte>(value >> 8U));
  out.push_back(static_cast<Byte>(value));
}

void put_u32(Bytes& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<Byte>(value >> static_cast<unsigned>(shift)));
  }
}

void put_u64(Bytes& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<Byte>(value >> static_cast<unsigned>(shift)));
  }
}

namespace {
void require(ByteView data, std::size_t offset, std::size_t width) {
  if (offset > data.size() || width > data.size() - offset) {
    throw Error("truncated binary data");
  }
}
}  // namespace

std::uint16_t read_u16(ByteView data, std::size_t offset) {
  require(data, offset, 2);
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(data[offset]) << 8U) | data[offset + 1]);
}

std::uint32_t read_u32(ByteView data, std::size_t offset) {
  require(data, offset, 4);
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8U) | data[offset + i];
  }
  return value;
}

std::uint64_t read_u64(ByteView data, std::size_t offset) {
  require(data, offset, 8);
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | data[offset + i];
  }
  return value;
}

std::size_t common_prefix(ByteView lhs, ByteView rhs) noexcept {
  std::size_t size = 0;
  while (size < lhs.size() && size < rhs.size() && lhs[size] == rhs[size]) {
    ++size;
  }
  return size;
}

std::uint32_t checksum(ByteView data) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const Byte byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

}  // namespace tiny_lsm
