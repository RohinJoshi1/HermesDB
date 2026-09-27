#pragma once

#include "hermesdb/common.hpp"

#include <algorithm>
#include <compare>
#include <cstring>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define HERMESDB_SIMD_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define HERMESDB_SIMD_SSE2 1
#endif

namespace hermesdb {
namespace simd {

[[nodiscard]] inline bool equal16(const Byte* lhs, const Byte* rhs) noexcept {
#if defined(HERMESDB_SIMD_NEON)
  const uint8x16_t eq = vceqq_u8(vld1q_u8(lhs), vld1q_u8(rhs));
  const uint64x2_t packed = vreinterpretq_u64_u8(eq);
  return vgetq_lane_u64(packed, 0) == ~uint64_t{0} &&
         vgetq_lane_u64(packed, 1) == ~uint64_t{0};
#elif defined(HERMESDB_SIMD_SSE2)
  const __m128i eq =
      _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs)),
                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs)));
  return _mm_movemask_epi8(eq) == 0xffff;
#else
  return std::memcmp(lhs, rhs, 16) == 0;
#endif
}

[[nodiscard]] inline std::strong_ordering compare16(const Byte* lhs,
                                                    const Byte* rhs) noexcept {
#if defined(HERMESDB_SIMD_NEON)
  const uint8x16_t va = vld1q_u8(lhs);
  const uint8x16_t vb = vld1q_u8(rhs);
  const uint8x16_t eq = vceqq_u8(va, vb);
  const uint64x2_t eq64 = vreinterpretq_u64_u8(eq);
  if (vgetq_lane_u64(eq64, 0) == ~uint64_t{0} &&
      vgetq_lane_u64(eq64, 1) == ~uint64_t{0}) {
    return std::strong_ordering::equal;
  }
  const uint64x2_t ne = vreinterpretq_u64_u8(vmvnq_u8(eq));
  const std::uint64_t lo = vgetq_lane_u64(ne, 0);
  const unsigned idx =
      lo != 0 ? static_cast<unsigned>(__builtin_ctzll(lo) / 8)
              : 8u + static_cast<unsigned>(__builtin_ctzll(vgetq_lane_u64(ne, 1)) /
                                           8);
  return lhs[idx] <=> rhs[idx];
#elif defined(HERMESDB_SIMD_SSE2)
  const __m128i va =
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs));
  const __m128i vb =
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs));
  const int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb));
  if (mask == 0xffff) return std::strong_ordering::equal;
  const unsigned idx = static_cast<unsigned>(
      __builtin_ctz(static_cast<unsigned>(~mask) & 0xffffu));
  return lhs[idx] <=> rhs[idx];
#else
  const int cmp = std::memcmp(lhs, rhs, 16);
  if (cmp < 0) return std::strong_ordering::less;
  if (cmp > 0) return std::strong_ordering::greater;
  return std::strong_ordering::equal;
#endif
}

[[nodiscard]] inline std::strong_ordering compare_equal_len(
    const Byte* lhs, const Byte* rhs, std::size_t n) noexcept {
  std::size_t i = 0;
  while (i + 16 <= n) {
    if (const auto order = compare16(lhs + i, rhs + i); order != 0) {
      return order;
    }
    i += 16;
  }
  if (i < n) {
    const int cmp = std::memcmp(lhs + i, rhs + i, n - i);
    if (cmp < 0) return std::strong_ordering::less;
    if (cmp > 0) return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

}  // namespace simd

[[nodiscard]] inline std::strong_ordering compare_bytes(ByteView lhs,
                                                        ByteView rhs) noexcept {
  const std::size_t n = std::min(lhs.size(), rhs.size());
  if (n != 0) {
    if (const auto order = simd::compare_equal_len(lhs.data(), rhs.data(), n);
        order != 0) {
      return order;
    }
  }
  return lhs.size() <=> rhs.size();
}

}  // namespace hermesdb
