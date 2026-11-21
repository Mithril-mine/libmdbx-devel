// > dist-cutoff-begin
#pragma once
#include "decl-core.h++"
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
    return bytes ? ::std::memcmp(a, b, bytes) : 0;
}

//------------------------------------------------------------------------------

MDBX_CXX11_CONSTEXPR map_handle::info::info(map_handle::flags flags, map_handle::state state) noexcept
    : flags(flags), state(state) {}

MDBX_CXX11_CONSTEXPR_ENUM mdbx::key_mode map_handle::info::key_mode() const noexcept {
  /* Only the combinations accepted by the core (see check_table_flags()) are
   * the declared members; the both-bits combination is rejected by the core
   * and mapped to the ordinal fallback to never produce an undeclared value.
   * The mask is switched as an integer to avoid -Wswitch on the combined
   * cases which are not declared enumerators of MDBX_db_flags_t. */
  switch (unsigned(flags) & unsigned(MDBX_REVERSEKEY | MDBX_INTEGERKEY)) {
  case unsigned(MDBX_REVERSEKEY):
    return mdbx::key_mode::reverse;
  default /* MDBX_DB_DEFAULTS, MDBX_INTEGERKEY, or the rejected both-bits combination */:
    return (flags & MDBX_INTEGERKEY) ? mdbx::key_mode::ordinal : mdbx::key_mode::usual;
  }
}

MDBX_CXX11_CONSTEXPR_ENUM mdbx::value_mode map_handle::info::value_mode() const noexcept {
  /* Only the combinations accepted by the core (see check_table_flags()) are
   * the declared members, except the reversed-ordinal one which has no
   * dedicated member; any other combination is invalid and mapped to the
   * closest defined mode to never produce an undeclared value. The mask is
   * switched as an integer to avoid -Wswitch on the combined cases which
   * are not declared enumerators of MDBX_db_flags_t. */
  switch (unsigned(flags) & unsigned(MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_DUPFIXED | MDBX_INTEGERDUP)) {
  case unsigned(MDBX_DUPSORT):
    return mdbx::value_mode::multi;
  case unsigned(MDBX_DUPSORT | MDBX_REVERSEDUP):
    return mdbx::value_mode::multi_reverse;
  case unsigned(MDBX_DUPSORT | MDBX_DUPFIXED):
    return mdbx::value_mode::multi_samelength;
  case unsigned(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP):
    return mdbx::value_mode::multi_reverse_samelength;
  case unsigned(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP):
  case unsigned(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_REVERSEDUP):
    return mdbx::value_mode::multi_ordinal;
  default /* invalid combinations: duplicate-related flags are meaningless
             without MDBX_DUPSORT, MDBX_INTEGERDUP requires MDBX_DUPFIXED */:
    return (flags & MDBX_DUPSORT) ? mdbx::value_mode::multi : mdbx::value_mode::single;
  }
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
