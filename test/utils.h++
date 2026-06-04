/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#pragma once
#include "base.h++"

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__)
#error __BYTE_ORDER__ should be defined.
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ && __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error Unsupported byte order.
#endif

#define is_byteorder_le() (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define is_byteorder_be() (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

//-----------------------------------------------------------------------------

#ifndef rot64
static inline uint64_t rot64(uint64_t v, unsigned s) { return (v >> s) | (v << (64 - s)); }
#endif /* rot64 */

static inline bool is_power2(size_t x) { return (x & (x - 1)) == 0; }

#undef roundup2
static inline size_t roundup2(size_t value, size_t granularity) {
  assert(is_power2(granularity));
  return (value + granularity - 1) & ~(granularity - 1);
}

//-----------------------------------------------------------------------------

static inline void memory_barrier(void) {
#if __has_extension(c_atomic) || __has_extension(cxx_atomic)
  __c11_atomic_thread_fence(__ATOMIC_SEQ_CST);
#elif defined(__ATOMIC_SEQ_CST)
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#elif defined(__clang__) || defined(__GNUC__)
  __sync_synchronize();
#elif defined(_MSC_VER)
  MemoryBarrier();
#elif defined(__INTEL_COMPILER) /* LY: Intel Compiler may mimic GCC and MSC */
#if defined(__ia64__) || defined(__ia64) || defined(_M_IA64)
  __mf();
#elif defined(__ia32__)
  _mm_mfence();
#else
#error "Unknown target for Intel Compiler, please report to us."
#endif
#elif defined(__SUNPRO_C) || defined(__sun) || defined(sun)
  __machine_rw_barrier();
#elif (defined(_HPUX_SOURCE) || defined(__hpux) || defined(__HP_aCC)) && (defined(HP_IA64) || defined(__ia64))
  _Asm_mf();
#elif defined(_AIX) || defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
  __lwsync();
#else
#error "Could not guess the kind of compiler, please report to us."
#endif
}

static inline void cpu_relax() {
#if defined(__ia32__)
  _mm_pause();
#elif defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS) || defined(YieldProcessor)
  YieldProcessor();
#else
/* nope */
#endif
}

//-----------------------------------------------------------------------------

struct simple_checksum {
  uint64_t value{0};

  simple_checksum() = default;

  void push(const uint32_t &data) {
    value += data * UINT64_C(9386433910765580089) + 1;
    value ^= value >> 41;
    value *= UINT64_C(0xBD9CACC22C6E9571);
  }

  void push(const uint64_t &data) {
    push((uint32_t)data);
    push((uint32_t)(data >> 32));
  }

  void push(const bool data) { push(data ? UINT32_C(0x780E) : UINT32_C(0xFA18E)); }

  void push(const void *ptr, size_t bytes) {
    const uint8_t *data = (const uint8_t *)ptr;
    for (size_t i = 0; i < bytes; ++i)
      push((uint32_t)data[i]);
  }

  void push(const double &data) { push(&data, sizeof(double)); }
  void push(const char *cstr) { push(cstr, strlen(cstr)); }
  void push(const std::string &str) { push(str.data(), str.size()); }

  void push(unsigned salt, const MDBX_val &val) {
    push(unsigned(val.iov_len));
    push(salt);
    push(val.iov_base, val.iov_len);
  }

#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
  void push(const HANDLE &handle) { push(&handle, sizeof(handle)); }
#endif /* _WINDOWS */
};

std::string data2hex(const void *ptr, size_t bytes, simple_checksum &checksum);
bool hex2data(const char *hex_begin, const char *hex_end, void *ptr, size_t bytes, simple_checksum &checksum);
bool is_samedata(const MDBX_val *a, const MDBX_val *b);
inline bool is_samedata(const MDBX_val &a, const MDBX_val &b) { return is_samedata(&a, &b); }
std::string format(const char *fmt, ...);

static inline uint64_t bleach64(uint64_t x) {
  // NASAM from Tommy Ettinger,
  // https://www.blogger.com/profile/04953541827437796598
  // http://mostlymangling.blogspot.com/2020/01/nasam-not-another-strange-acronym-mixer.html
  x ^= rot64(x, 25) ^ rot64(x, 47);
  x *= UINT64_C(0x9E6C63D0676A9A99);
  x ^= x >> 23 ^ x >> 51;
  x *= UINT64_C(0x9E6D62D06F6A9A9B);
  x ^= x >> 23 ^ x >> 51;
  return x;
}

static inline uint32_t bleach32(uint32_t x) {
  // https://github.com/skeeto/hash-prospector
  // exact bias: 0.10760229515479501
  x ^= x >> 16;
  x *= UINT32_C(0x21f0aaad);
  x ^= 0x3027C563 ^ (x >> 15);
  x *= UINT32_C(0x0d35a2d97);
  x ^= x >> 15;
  return x;
}

static inline uint64_t prng64_map1_careless(uint64_t state) { return state * UINT64_C(6364136223846793005) + 1; }

static inline uint64_t prng64_map2_careless(uint64_t state) {
  return (state + UINT64_C(1442695040888963407)) * UINT64_C(6364136223846793005);
}

static inline uint64_t prng64_map1_white(uint64_t state) { return bleach64(prng64_map1_careless(state)); }

static inline uint64_t prng64_map2_white(uint64_t state) { return bleach64(prng64_map2_careless(state)); }

static inline uint64_t prng64_careless(uint64_t &state) {
  state = prng64_map1_careless(state);
  return state;
}

static inline double u64_to_double1(uint64_t v) {
  union {
    uint64_t u64;
    double d;
  } casting;

  casting.u64 = UINT64_C(0x3ff) << 52 | (v >> 12);
  assert(casting.d >= 1.0 && casting.d < 2.0);
  return casting.d - 1.0;
}

uint64_t prng64_white(uint64_t &state);
uint32_t prng32_white(uint64_t &state);
uint32_t prng32_fast(uint64_t &state);
void prng_fill(uint64_t &state, void *ptr, size_t bytes);

extern uint64_t prng_state;
void prng_seed(uint64_t seed);
void prng_salt(unsigned salt);
uint32_t prng32(void);
uint64_t prng64(void);
void prng_fill(void *ptr, size_t bytes);
inline uint32_t prng32_range(unsigned minimal, unsigned miximal) {
  return (minimal < miximal) ? minimal + prng32() % (miximal - minimal) : miximal;
}

bool flipcoin();
bool flipcoin_x2();
bool flipcoin_x3();
bool flipcoin_x4();
bool flipcoin_n(unsigned n);
bool jitter(unsigned probability_percent);
void jitter_delay(bool extra = false);
