// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief Cache entry for get-cached API (initial draft).
class cache_entry : public MDBX_cache_entry_t {
public:
  cache_entry() noexcept { reset(); }
  cache_entry(const cache_entry &) noexcept = default;
  cache_entry &operator=(const cache_entry &) noexcept = default;
  cache_entry(cache_entry &&other) noexcept {
    *this = other;
    other.reset();
  }
  void reset() noexcept { mdbx_cache_init(this); }
};

//------------------------------------------------------------------------------

/// \brief Loop control constants for readers enumeration functor and other
/// cases. \see env::enumerate_readers()
enum loop_control { continue_loop = 0, exit_loop = INT32_MIN };

/// \brief Kinds of the keys and corresponding modes of comparing it.
enum class key_mode {
  usual = MDBX_DB_DEFAULTS,  ///< Usual variable length keys with byte-by-byte
                             ///< lexicographic comparison like `std::memcmp()`.
  reverse = MDBX_REVERSEKEY, ///< Variable length keys with byte-by-byte
                             ///< lexicographic comparison in reverse order,
                             ///< from the end of the keys to the beginning.
  ordinal = MDBX_INTEGERKEY, ///< Keys are binary integers in native byte order,
                             ///< either `uint32_t` or `uint64_t`, and will be
                             ///< sorted as such. The keys must all be of the
                             ///< same size and must be aligned while passing
                             ///< as arguments.
  msgpack = -1               ///< Keys are in [MessagePack](https://msgpack.org/)
                             ///< format with appropriate comparison.
                             ///< \note Not yet implemented and PRs are welcome.
};

MDBX_CXX01_CONSTEXPR_ENUM bool is_usual(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & (MDBX_REVERSEKEY | MDBX_INTEGERKEY)) == 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_ordinal(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERKEY) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_samelength(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERKEY) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_reverse(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_REVERSEKEY) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_msgpack(key_mode mode) noexcept { return mode == key_mode::msgpack; }

/// \brief Kind of the values and sorted multi-values with corresponding comparison.
enum class value_mode {
  single = MDBX_DB_DEFAULTS, ///< Usual single value for each key. In terms of
                             ///< keys, they are unique.
  multi = MDBX_DUPSORT,      ///< A more than one data value could be associated with
                             ///< each key. Internally each key is stored once, and the
                             ///< corresponding data values are sorted by byte-by-byte
                             ///< lexicographic comparison like `std::memcmp()`.
                             ///< In terms of keys, they are not unique, i.e. has
                             ///< duplicates which are sorted by associated data values.
#if CONSTEXPR_ENUM_FLAGS_OPERATIONS || defined(DOXYGEN)
  multi_reverse = MDBX_DUPSORT | MDBX_REVERSEDUP,  ///< A more than one data value could be associated with
                                                   ///< each key. Internally each key is stored once, and
                                                   ///< the corresponding data values are sorted by
                                                   ///< byte-by-byte lexicographic comparison in reverse
                                                   ///< order, from the end of the keys to the beginning.
                                                   ///< In terms of keys, they are not unique, i.e. has
                                                   ///< duplicates which are sorted by associated data
                                                   ///< values.
  multi_samelength = MDBX_DUPSORT | MDBX_DUPFIXED, ///< A more than one data value could be associated with
                                                   ///< each key, and all data values must be same length.
                                                   ///< Internally each key is stored once, and the
                                                   ///< corresponding data values are sorted by byte-by-byte
                                                   ///< lexicographic comparison like `std::memcmp()`. In
                                                   ///< terms of keys, they are not unique, i.e. has
                                                   ///< duplicates which are sorted by associated data values.
  multi_ordinal = MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP, ///< A more than one data value could be associated
                                                                  ///< with each key, and all data values are binary
                                                                  ///< integers in native byte order, either `uint32_t`
                                                                  ///< or `uint64_t`, and will be sorted as such.
                                                                  ///< Internally each key is stored once, and the
                                                                  ///< corresponding data values are sorted. In terms of
                                                                  ///< keys, they are not unique, i.e. has duplicates
                                                                  ///< which are sorted by associated data values.
  multi_reverse_samelength =
      MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_DUPFIXED, ///< A more than one data value could be associated with
                                                      ///< each key, and all data values must be same length.
                                                      ///< Internally each key is stored once, and the
                                                      ///< corresponding data values are sorted by byte-by-byte
                                                      ///< lexicographic comparison in reverse order, from the
                                                      ///< end of the keys to the beginning. In terms of keys,
                                                      ///< they are not unique, i.e. has duplicates which are
                                                      ///< sorted by associated data values.
#else
  multi_reverse = uint32_t(MDBX_DUPSORT) | uint32_t(MDBX_REVERSEDUP),
  multi_samelength = uint32_t(MDBX_DUPSORT) | uint32_t(MDBX_DUPFIXED),
  multi_ordinal = uint32_t(MDBX_DUPSORT) | uint32_t(MDBX_DUPFIXED) | uint32_t(MDBX_INTEGERDUP),
  multi_reverse_samelength = uint32_t(MDBX_DUPSORT) | uint32_t(MDBX_REVERSEDUP) | uint32_t(MDBX_DUPFIXED),
#endif
  msgpack = -1 ///< A more than one data value could be associated with each
  ///< key. Values are in [MessagePack](https://msgpack.org/)
  ///< format with appropriate comparison. Internally each key is
  ///< stored once, and the corresponding data values are sorted.
  ///< In terms of keys, they are not unique, i.e. has duplicates
  ///< which are sorted by associated data values.
  ///< \note Not yet implemented and PRs are welcome.
};

MDBX_CXX01_CONSTEXPR_ENUM bool is_usual(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & (MDBX_DUPSORT | MDBX_INTEGERDUP | MDBX_DUPFIXED | MDBX_REVERSEDUP)) == 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_multi(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_DUPSORT) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_ordinal(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERDUP) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_samelength(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_DUPFIXED) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_reverse(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_REVERSEDUP) != 0;
}

MDBX_CXX01_CONSTEXPR_ENUM bool is_msgpack(value_mode mode) noexcept { return mode == value_mode::msgpack; }

/// \brief A handle for an individual table (aka key-value space, maps or sub-database) in the environment.
/// \see txn::open_map() \see txn::create_map()
/// \see txn::clear_map() \see txn::drop_map()
/// \see txn::get_map_flags() \see txn::get_map_stat()
/// \see env::close_map()
/// \see cursor::map()
struct LIBMDBX_API_TYPE map_handle {
  MDBX_dbi dbi{0};
  MDBX_CXX11_CONSTEXPR map_handle() noexcept {}
  MDBX_CXX11_CONSTEXPR map_handle(MDBX_dbi dbi) noexcept : dbi(dbi) {}
  map_handle(const map_handle &) noexcept = default;
  map_handle &operator=(const map_handle &) noexcept = default;
  operator bool() const noexcept { return dbi != 0; }
  operator MDBX_dbi() const { return dbi; }

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
  friend MDBX_CXX11_CONSTEXPR auto operator<=>(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi <=> b.dbi;
  }
#endif /* __cpp_impl_three_way_comparison */
  friend MDBX_CXX14_CONSTEXPR bool operator==(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi == b.dbi;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator<(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi < b.dbi;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator>(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi > b.dbi;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator<=(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi <= b.dbi;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator>=(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi >= b.dbi;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator!=(const map_handle &a, const map_handle &b) noexcept {
    return a.dbi != b.dbi;
  }

  using flags = ::MDBX_db_flags_t;
  using state = ::MDBX_dbi_state_t;
  struct LIBMDBX_API_TYPE info {
    map_handle::flags flags;
    map_handle::state state;
    MDBX_CXX11_CONSTEXPR info(map_handle::flags flags, map_handle::state state) noexcept;
    info(const info &) noexcept = default;
    info &operator=(const info &) noexcept = default;
    MDBX_CXX11_CONSTEXPR_ENUM mdbx::key_mode key_mode() const noexcept;
    MDBX_CXX11_CONSTEXPR_ENUM mdbx::value_mode value_mode() const noexcept;
  };
};

using comparator = ::MDBX_cmp_func *;
inline comparator default_comparator(key_mode mode) noexcept {
  return ::mdbx_get_keycmp(static_cast<MDBX_db_flags_t>(mode));
}
inline comparator default_comparator(value_mode mode) noexcept {
  return ::mdbx_get_keycmp(static_cast<MDBX_db_flags_t>(mode));
}

/// \brief Key-value pairs put mode.
enum put_mode {
  insert_unique = MDBX_NOOVERWRITE, ///< Insert only unique keys.
  upsert = MDBX_UPSERT,             ///< Insert or update.
  update = MDBX_CURRENT,            ///< Update existing, don't insert new.
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
