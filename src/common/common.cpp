#include "hermesdb/common.hpp"

#include <algorithm>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace hermesdb {

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

namespace {

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
std::uint32_t crc32_hw(ByteView data) noexcept {
  std::uint32_t crc = 0xffffffffU;
  std::size_t i = 0;
  while (i + 8 <= data.size()) {
    std::uint64_t word = 0;
    std::memcpy(&word, data.data() + i, 8);
    crc = __crc32d(crc, word);
    i += 8;
  }
  while (i < data.size()) {
    crc = __crc32b(crc, data[i]);
    ++i;
  }
  return ~crc;
}
#endif

struct Crc32Table {
  std::uint32_t t[8][256]{};
  Crc32Table() {
    for (int i = 0; i < 256; ++i) {
      auto crc = static_cast<std::uint32_t>(i);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
      t[0][i] = crc;
    }
    for (int i = 0; i < 256; ++i) {
      auto crc = t[0][i];
      for (int k = 1; k < 8; ++k) {
        crc = t[0][crc & 0xffU] ^ (crc >> 8U);
        t[k][i] = crc;
      }
    }
  }
};

const Crc32Table& crc32_table() {
  static const Crc32Table table;
  return table;
}

[[maybe_unused]] std::uint32_t crc32_slice8(ByteView data) noexcept {
  const auto& tab = crc32_table();
  std::uint32_t crc = 0xffffffffU;
  std::size_t i = 0;
  while (i + 8 <= data.size()) {
    crc ^= static_cast<std::uint32_t>(data[i]) |
           (static_cast<std::uint32_t>(data[i + 1]) << 8U) |
           (static_cast<std::uint32_t>(data[i + 2]) << 16U) |
           (static_cast<std::uint32_t>(data[i + 3]) << 24U);
    const auto next = static_cast<std::uint32_t>(data[i + 4]) |
                      (static_cast<std::uint32_t>(data[i + 5]) << 8U) |
                      (static_cast<std::uint32_t>(data[i + 6]) << 16U) |
                      (static_cast<std::uint32_t>(data[i + 7]) << 24U);
    crc = tab.t[7][crc & 0xffU] ^ tab.t[6][(crc >> 8U) & 0xffU] ^
          tab.t[5][(crc >> 16U) & 0xffU] ^ tab.t[4][crc >> 24U] ^
          tab.t[3][next & 0xffU] ^ tab.t[2][(next >> 8U) & 0xffU] ^
          tab.t[1][(next >> 16U) & 0xffU] ^ tab.t[0][next >> 24U];
    i += 8;
  }
  while (i < data.size()) {
    crc = tab.t[0][(crc ^ data[i]) & 0xffU] ^ (crc >> 8U);
    ++i;
  }
  return ~crc;
}

}  // namespace

std::uint32_t checksum(ByteView data) noexcept {
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  return crc32_hw(data);
#else
  return crc32_slice8(data);
#endif
}

}  // namespace hermesdb
