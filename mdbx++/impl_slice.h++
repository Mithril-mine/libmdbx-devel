// > dist-cutoff-begin
#pragma once
#include "decl_slice.h++"
#include "decl_transcoders.h++"
namespace mdbx {
// < dist-cutoff-end

MDBX_CXX11_CONSTEXPR slice::slice() noexcept : ::MDBX_val({nullptr, 0}) {}

MDBX_CXX14_CONSTEXPR slice::slice(const void *ptr, size_t bytes)
    : ::MDBX_val({const_cast<void *>(ptr), check_length(bytes)}) {}

MDBX_CXX14_CONSTEXPR slice::slice(const void *begin, const void *end)
    : slice(begin, static_cast<const byte *>(end) - static_cast<const byte *>(begin)) {}

MDBX_CXX17_CONSTEXPR slice::slice(const char *c_str) : slice(c_str, strlen(c_str)) {}

MDBX_CXX14_CONSTEXPR slice::slice(const MDBX_val &src) : slice(src.iov_base, src.iov_len) {}

MDBX_CXX14_CONSTEXPR slice::slice(MDBX_val &&src) : slice(src) { src.iov_base = nullptr; }

MDBX_CXX14_CONSTEXPR slice::slice(slice &&src) noexcept : slice(src) { src.invalidate(); }

inline slice &slice::assign(const void *ptr, size_t bytes) {
  iov_base = const_cast<void *>(ptr);
  iov_len = check_length(bytes);
  return *this;
}

inline slice &slice::assign(const slice &src) noexcept {
  iov_base = src.iov_base;
  iov_len = src.iov_len;
  return *this;
}

inline slice &slice::assign(const ::MDBX_val &src) { return assign(src.iov_base, src.iov_len); }

slice &slice::assign(slice &&src) noexcept {
  assign(src);
  src.invalidate();
  return *this;
}

inline slice &slice::assign(::MDBX_val &&src) {
  assign(src.iov_base, src.iov_len);
  src.iov_base = nullptr;
  return *this;
}

inline slice &slice::assign(const void *begin, const void *end) {
  return assign(begin, static_cast<const byte *>(end) - static_cast<const byte *>(begin));
}

inline slice &slice::assign(const char *c_str) { return assign(c_str, strlen(c_str)); }

inline slice &slice::operator=(slice &&src) noexcept { return assign(::std::move(src)); }

inline slice &slice::operator=(::MDBX_val &&src) { return assign(::std::move(src)); }

MDBX_CXX11_CONSTEXPR const byte *slice::byte_ptr() const noexcept { return static_cast<const byte *>(iov_base); }

MDBX_CXX11_CONSTEXPR const byte *slice::end_byte_ptr() const noexcept { return byte_ptr() + length(); }

MDBX_CXX11_CONSTEXPR byte *slice::byte_ptr() noexcept { return static_cast<byte *>(iov_base); }

MDBX_CXX11_CONSTEXPR byte *slice::end_byte_ptr() noexcept { return byte_ptr() + length(); }

MDBX_CXX11_CONSTEXPR const char *slice::char_ptr() const noexcept { return static_cast<const char *>(iov_base); }

MDBX_CXX11_CONSTEXPR const char *slice::end_char_ptr() const noexcept { return char_ptr() + length(); }

MDBX_CXX11_CONSTEXPR char *slice::char_ptr() noexcept { return static_cast<char *>(iov_base); }

MDBX_CXX11_CONSTEXPR char *slice::end_char_ptr() noexcept { return char_ptr() + length(); }

MDBX_CXX11_CONSTEXPR const void *slice::data() const noexcept { return iov_base; }

MDBX_CXX11_CONSTEXPR const void *slice::end() const noexcept { return static_cast<const void *>(end_byte_ptr()); }

MDBX_CXX11_CONSTEXPR void *slice::data() noexcept { return iov_base; }

MDBX_CXX11_CONSTEXPR void *slice::end() noexcept { return static_cast<void *>(end_byte_ptr()); }

MDBX_CXX11_CONSTEXPR size_t slice::length() const noexcept { return iov_len; }

MDBX_CXX14_CONSTEXPR slice &slice::set_length(size_t bytes) {
  iov_len = check_length(bytes);
  return *this;
}

MDBX_CXX14_CONSTEXPR slice &slice::set_end(const void *ptr) {
  MDBX_CONSTEXPR_ASSERT(static_cast<const char *>(ptr) >= char_ptr());
  return set_length(static_cast<const char *>(ptr) - char_ptr());
}

MDBX_CXX11_CONSTEXPR bool slice::empty() const noexcept { return length() == 0; }

MDBX_CXX11_CONSTEXPR bool slice::is_null() const noexcept { return data() == nullptr; }

MDBX_CXX11_CONSTEXPR size_t slice::size() const noexcept { return length(); }

MDBX_CXX11_CONSTEXPR slice::operator bool() const noexcept { return !is_null(); }

MDBX_CXX14_CONSTEXPR void slice::invalidate() noexcept { iov_base = nullptr; }

MDBX_CXX14_CONSTEXPR void slice::clear() noexcept {
  iov_base = nullptr;
  iov_len = 0;
}

inline void slice::remove_prefix(size_t n) noexcept {
  assert(n <= size());
  iov_base = static_cast<byte *>(iov_base) + n;
  iov_len -= n;
}

inline void slice::safe_remove_prefix(size_t n) {
  if (MDBX_UNLIKELY(n > size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  remove_prefix(n);
}

inline void slice::remove_suffix(size_t n) noexcept {
  assert(n <= size());
  iov_len -= n;
}

inline void slice::safe_remove_suffix(size_t n) {
  if (MDBX_UNLIKELY(n > size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  remove_suffix(n);
}

MDBX_CXX14_CONSTEXPR bool slice::starts_with(const slice &prefix) const noexcept {
  return length() >= prefix.length() && memcmp(data(), prefix.data(), prefix.length()) == 0;
}

MDBX_CXX14_CONSTEXPR bool slice::ends_with(const slice &suffix) const noexcept {
  return length() >= suffix.length() &&
         memcmp(byte_ptr() + length() - suffix.length(), suffix.data(), suffix.length()) == 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t slice::hash_value() const noexcept {
  size_t h = length() * 3977471;
  for (size_t i = 0; i < length(); ++i)
    h = (h ^ static_cast<const uint8_t *>(data())[i]) * 1664525 + 1013904223;
  return h ^ 3863194411 * (h >> 11);
}

MDBX_CXX11_CONSTEXPR byte slice::operator[](size_t n) const noexcept {
  MDBX_CONSTEXPR_ASSERT(n < size());
  return byte_ptr()[n];
}

MDBX_CXX11_CONSTEXPR byte slice::at(size_t n) const {
  if (MDBX_UNLIKELY(n >= size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  return byte_ptr()[n];
}

MDBX_CXX14_CONSTEXPR slice slice::head(size_t n) const noexcept {
  MDBX_CONSTEXPR_ASSERT(n <= size());
  return slice(data(), n);
}

MDBX_CXX14_CONSTEXPR slice slice::tail(size_t n) const noexcept {
  MDBX_CONSTEXPR_ASSERT(n <= size());
  return slice(char_ptr() + size() - n, n);
}

MDBX_CXX14_CONSTEXPR slice slice::middle(size_t from, size_t n) const noexcept {
  MDBX_CONSTEXPR_ASSERT(from + n <= size());
  return slice(char_ptr() + from, n);
}

MDBX_CXX14_CONSTEXPR slice slice::safe_head(size_t n) const {
  if (MDBX_UNLIKELY(n > size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  return head(n);
}

MDBX_CXX14_CONSTEXPR slice slice::safe_tail(size_t n) const {
  if (MDBX_UNLIKELY(n > size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  return tail(n);
}

MDBX_CXX14_CONSTEXPR slice slice::safe_middle(size_t from, size_t n) const {
  if (MDBX_UNLIKELY(n > max_length))
    MDBX_CXX20_UNLIKELY throw_max_length_exceeded();
  if (MDBX_UNLIKELY(from + n > size()))
    MDBX_CXX20_UNLIKELY throw_out_range();
  return middle(from, n);
}

MDBX_CXX14_CONSTEXPR intptr_t slice::compare_fast(const slice &a, const slice &b) noexcept {
  const intptr_t diff = intptr_t(a.length()) - intptr_t(b.length());
  return diff                                                     ? diff
         : MDBX_UNLIKELY(a.length() == 0 || a.data() == b.data()) ? 0
                                                                  : memcmp(a.data(), b.data(), a.length());
}

MDBX_CXX14_CONSTEXPR intptr_t slice::compare_lexicographically(const slice &a, const slice &b) noexcept {
  const size_t shortest = ::std::min(a.length(), b.length());
  if (MDBX_LIKELY(shortest > 0))
    MDBX_CXX20_LIKELY {
      const intptr_t diff = memcmp(a.data(), b.data(), shortest);
      if (MDBX_LIKELY(diff != 0))
        MDBX_CXX20_LIKELY return diff;
    }
  return intptr_t(a.length()) - intptr_t(b.length());
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator==(const slice &a, const slice &b) noexcept {
  return slice::compare_fast(a, b) == 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator<(const slice &a, const slice &b) noexcept {
  return slice::compare_lexicographically(a, b) < 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator>(const slice &a, const slice &b) noexcept {
  return slice::compare_lexicographically(a, b) > 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator<=(const slice &a, const slice &b) noexcept {
  return slice::compare_lexicographically(a, b) <= 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator>=(const slice &a, const slice &b) noexcept {
  return slice::compare_lexicographically(a, b) >= 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator!=(const slice &a, const slice &b) noexcept {
  return slice::compare_fast(a, b) != 0;
}

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR auto operator<=>(const slice &a, const slice &b) noexcept {
  return slice::compare_lexicographically(a, b);
}
#endif /* __cpp_impl_three_way_comparison */

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
