// > dist-cutoff-begin
#pragma once
#include "decl_core.h++"
namespace mdbx {
// < dist-cutoff-end

MDBX_CXX11_CONSTEXPR const version_info &get_version() noexcept { return ::mdbx_version; }
MDBX_CXX11_CONSTEXPR const build_info &get_build() noexcept { return ::mdbx_build; }

static MDBX_CXX17_CONSTEXPR size_t strlen(const char *c_str) noexcept {
#if defined(__cpp_lib_is_constant_evaluated) && __cpp_lib_is_constant_evaluated >= 201811L
  if (::std::is_constant_evaluated()) {
    size_t i = 0;
    if (c_str)
      while (c_str[i])
        ++i;
    return i;
  }
#endif /* __cpp_lib_is_constant_evaluated >= 201811 */
#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L
  return c_str ? ::std::string_view(c_str).length() : 0;
#else
  return c_str ? ::std::strlen(c_str) : 0;
#endif
}

MDBX_MAYBE_UNUSED static MDBX_CXX20_CONSTEXPR void *memcpy(void *dest, const void *src, size_t bytes) noexcept {
#if defined(__cpp_lib_is_constant_evaluated) && __cpp_lib_is_constant_evaluated >= 201811L
  if (::std::is_constant_evaluated()) {
    for (size_t i = 0; i < bytes; ++i)
      static_cast<byte *>(dest)[i] = static_cast<const byte *>(src)[i];
    return dest;
  } else
#endif /* __cpp_lib_is_constant_evaluated >= 201811 */
    return ::std::memcpy(dest, src, bytes);
}

static MDBX_CXX20_CONSTEXPR int memcmp(const void *a, const void *b, size_t bytes) noexcept {
#if defined(__cpp_lib_is_constant_evaluated) && __cpp_lib_is_constant_evaluated >= 201811L
  if (::std::is_constant_evaluated()) {
    for (size_t i = 0; i < bytes; ++i) {
      const int diff = int(static_cast<const byte *>(a)[i]) - int(static_cast<const byte *>(b)[i]);
      if (diff)
        return diff;
    }
    return 0;
  } else
#endif /* __cpp_lib_is_constant_evaluated >= 201811 */
    return ::std::memcmp(a, b, bytes);
}

//------------------------------------------------------------------------------

MDBX_CXX11_CONSTEXPR map_handle::info::info(map_handle::flags flags, map_handle::state state) noexcept
    : flags(flags), state(state) {}

MDBX_CXX11_CONSTEXPR_ENUM mdbx::key_mode map_handle::info::key_mode() const noexcept {
  return ::mdbx::key_mode(flags & (MDBX_REVERSEKEY | MDBX_INTEGERKEY));
}

MDBX_CXX11_CONSTEXPR_ENUM mdbx::value_mode map_handle::info::value_mode() const noexcept {
  return ::mdbx::value_mode(flags & (MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_DUPFIXED | MDBX_INTEGERDUP));
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
