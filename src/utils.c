/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

size_t clz64_fallback(uint64_t value) {
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value |= value >> 32;
  static const uint8_t deBruijn_clz64[64] = {63, 16, 62, 7,  15, 36, 61, 3,  6,  14, 22, 26, 35, 47, 60, 2,
                                             9,  5,  28, 11, 13, 21, 42, 19, 25, 31, 34, 40, 46, 52, 59, 1,
                                             17, 8,  37, 4,  23, 27, 48, 10, 29, 12, 43, 20, 32, 41, 53, 18,
                                             38, 24, 49, 30, 44, 33, 54, 39, 50, 45, 55, 51, 56, 57, 58, 0};
  return deBruijn_clz64[value * UINT64_C(0x03F79D71B4CB0A89) >> 58];
}

size_t clz32_fallback(uint32_t value) {
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  static const uint8_t deBruijn_clz32[32] = {31, 22, 30, 21, 18, 10, 29, 2,  20, 17, 15, 13, 9, 6,  28, 1,
                                             23, 19, 11, 3,  16, 14, 7,  24, 12, 4,  8,  25, 5, 26, 27, 0};
  return deBruijn_clz32[value * UINT32_C(0x07C4ACDD) >> 27];
}

size_t ctz64_fallback(uint64_t value) {
  static const uint8_t deBruijn_ctz64[64] = {0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28,
                                             62, 5,  39, 46, 44, 42, 22, 9,  24, 35, 59, 56, 49, 18, 29, 11,
                                             63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21, 23, 58, 17, 10,
                                             51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12};
  return deBruijn_ctz64[(UINT64_C(0x022FDD63CC95386D) * value) >> 58];
}

size_t ctz32_fallback(uint32_t value) {
  static const uint8_t deBruijn_ctz32[32] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                             31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
  return deBruijn_ctz32[(UINT32_C(0x077CB531) * value) >> 27];
}

//------------------------------------------------------------------------------

size_t ceil_log2n(size_t value_uintptr) {
  ASSERT(value_uintptr > 0 && value_uintptr < INT32_MAX);
  value_uintptr -= 1;
  value_uintptr |= value_uintptr >> 1;
  value_uintptr |= value_uintptr >> 2;
  value_uintptr |= value_uintptr >> 4;
  value_uintptr |= value_uintptr >> 8;
  value_uintptr |= value_uintptr >> 16;
  return log2n_powerof2(value_uintptr + 1);
}

uint64_t rrxmrrxmsx_0(uint64_t v) {
  /* Pelle Evensen's mixer, https://bit.ly/2HOfynt */
  v ^= (v << 39 | v >> 25) ^ (v << 14 | v >> 50);
  v *= UINT64_C(0xA24BAED4963EE407);
  v ^= (v << 40 | v >> 24) ^ (v << 15 | v >> 49);
  v *= UINT64_C(0x9FB21C651E98DF25);
  return v ^ v >> 28;
}

__cold char *ratio2digits(const uint64_t v, const uint64_t d, ratio2digits_buffer_t *const buffer, int precision) {
  ASSERT(d > 0 && precision < 20);
  char *const dot = buffer->string + 21;
  uint64_t i = v / d, f = v % d, m = d;

  char *tail = dot;
  bool carry = m - f < m / 2;
  if (precision > 0) {
    *tail = '.';
    do {
      while (unlikely(f > UINT64_MAX / 10)) {
        f >>= 1;
        m >>= 1;
      }
      f *= 10;
      ASSERT(tail > buffer->string && tail < ARRAY_END(buffer->string) - 1);
      *++tail = '0' + (char)(f / m);
      f %= m;
    } while (--precision && tail < ARRAY_END(buffer->string) - 1);

    carry = m - f < m / 2;
    for (char *scan = tail; carry && scan > dot; --scan)
      *scan = (carry = *scan == '9') ? '0' : *scan + 1;
  }
  ASSERT(tail > buffer->string && tail < ARRAY_END(buffer->string) - 1);
  *++tail = '\0';

  char *head = dot;
  i += carry;
  while (i > 9) {
    ASSERT(head > buffer->string && head < ARRAY_END(buffer->string));
    *--head = '0' + (char)(i % 10);
    i /= 10;
  }
  ASSERT(head > buffer->string && head < ARRAY_END(buffer->string));
  *--head = '0' + (char)i;

  return head;
}

__cold char *ratio2percent(uint64_t value, uint64_t whole, ratio2digits_buffer_t *buffer) {
  while (unlikely(value > UINT64_MAX / 100)) {
    value >>= 1;
    whole >>= 1;
  }
  const bool rough = whole >= value && (!value || value > whole / 16);
  return ratio2digits(value * 100, whole, buffer, rough ? 1 : 2);
}

MDBX_MAYBE_UNUSED bin128_t mul64x64_128_fallback(uint64_t x, uint64_t y) {
  bin128_t r;
#if MDBX_HAVE_NATIVE_U128 && MDBX_CHECKING < 1
  r.u128 = x;
  r.u128 *= y;
#else
  const uint64_t xl = x & UINT32_C(0xFFFFffff);
  const uint64_t xh = x >> 32;
  const uint64_t yl = y & UINT32_C(0xFFFFffff);
  const uint64_t yh = y >> 32;

  const uint64_t ll = xl * yl;
  const uint64_t hh = xh * yh;
  const uint64_t hl = xh * yl + (ll >> 32);
  const uint64_t lh = xl * yh + (hl & UINT32_C(0xFFFFffff));

  r.l = (lh << 32) | (ll & UINT32_C(0xFFFFffff));
  r.h = hh + (hl >> 32) + (lh >> 32);
#endif
  return r;
}
