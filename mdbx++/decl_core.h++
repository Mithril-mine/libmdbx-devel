// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief Cache entry for the transparent read-your-writes caching, see
/// txn::get_cached() and ::mdbx_cache_get().
///
/// \details A value type wrapping \ref MDBX_cache_entry_t. Each entry is
/// associated with a key and holds a lightweight "version stamp" plus the
/// offset of the cached value inside the memory-mapped database file, so
/// subsequent lookups stop as soon as they reach an unmodified page.
///
/// An entry must be initialized (\ref reset()) before the first use. Entries
/// may be placed in shared memory and used by multiple processes, though
/// libmdbx does not provide such interaction management yet.
class cache_entry : public MDBX_cache_entry_t {
public:
  /// \brief Constructs the entry and initializes it to the "no cached info" state.
  cache_entry() noexcept { reset(); }
  /// \brief Copy constructor.
  cache_entry(const cache_entry &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  cache_entry &operator=(const cache_entry &) noexcept = default;
  /// \brief Move constructor: copies the source entry and resets it.
  /// \param other  The source entry to be moved from.
  cache_entry(cache_entry &&other) noexcept {
    *this = other;
    other.reset();
  }

  /// \brief Initializes the cache entry to the "no cached info" state.
  void reset() noexcept { mdbx_cache_init(this); }

  /// \brief Returns `true` if the entry carries a confirmed result of a
  /// previous lookup, i.e. either a cached value or a cached "not found".
  /// \returns `true` if the entry is confirmed, `false` otherwise.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept { return last_confirmed_txnid != 0; }
  /// \brief Returns `true` if the entry caches the "key not found" state.
  /// \returns `true` if the entry caches the "not found" state.
  MDBX_CXX14_CONSTEXPR bool notfound() const noexcept { return offset == 0; }
  /// \brief Returns the transaction/MVCC-snapshot ID of the internal
  /// structure that holds the cached data.
  /// \returns The transaction ID of the internal structure.
  MDBX_CXX14_CONSTEXPR uint64_t transaction_id() const noexcept { return trunk_txnid; }
  /// \brief Returns the recent transaction/MVCC-snapshot ID wherein the
  /// entry was checked and confirmed.
  /// \returns The transaction ID of the last confirmation.
  MDBX_CXX14_CONSTEXPR uint64_t last_confirmed() const noexcept { return last_confirmed_txnid; }

  /// \brief Constructs the entry as a copy of the given C API structure.
  /// \param ce  The source \ref MDBX_cache_entry_t instance.
  MDBX_CXX20_CONSTEXPR cache_entry(const MDBX_cache_entry_t &ce) noexcept { mdbx::memcpy(this, &ce, sizeof(*this)); }
  /// \brief Assigns the given C API structure to this entry.
  /// \param ce  The source \ref MDBX_cache_entry_t instance.
  /// \returns A reference to this instance.
  MDBX_CXX20_CONSTEXPR cache_entry &operator=(const MDBX_cache_entry_t &ce) noexcept {
    mdbx::memcpy(this, &ce, sizeof(*this));
    return *this;
  };

  friend MDBX_CXX14_CONSTEXPR bool operator==(const cache_entry &a, const cache_entry &b) noexcept {
    return a.trunk_txnid == b.trunk_txnid && a.last_confirmed_txnid == b.last_confirmed_txnid && a.offset == b.offset &&
           a.length == b.length;
  }
  friend MDBX_CXX14_CONSTEXPR bool operator!=(const cache_entry &a, const cache_entry &b) noexcept { return !(a == b); }
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

/// \brief Checks whether the given key mode is the "usual" one.
/// \param mode  The key mode to be checked.
/// \returns `true` if the mode matches the "usual" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_usual(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & (MDBX_REVERSEKEY | MDBX_INTEGERKEY)) == 0;
}

/// \brief Checks whether the given key mode is the "ordinal" one.
/// \param mode  The key mode to be checked.
/// \returns `true` if the mode matches the "ordinal" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_ordinal(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERKEY) != 0;
}

/// \brief Checks whether the given key mode is the "same-length" one.
/// \param mode  The key mode to be checked.
/// \returns `true` if the mode matches the "same-length" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_samelength(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERKEY) != 0;
}

/// \brief Checks whether the given key mode is the "reverse" one.
/// \param mode  The key mode to be checked.
/// \returns `true` if the mode matches the "reverse" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_reverse(key_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_REVERSEKEY) != 0;
}

/// \brief Checks whether the given key mode is the "MessagePack" one.
/// \param mode  The key mode to be checked.
/// \returns `true` if the mode matches the "MessagePack" kind, `false` otherwise.
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
#if MDBX_CONSTEXPR_ENUM_FLAGS_OPERATIONS || defined(DOXYGEN)
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

/// \brief Checks whether the given value mode is the "usual" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "usual" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_usual(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & (MDBX_DUPSORT | MDBX_INTEGERDUP | MDBX_DUPFIXED | MDBX_REVERSEDUP)) == 0;
}

/// \brief Checks whether the given value mode is the "multi-value" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "multi-value" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_multi(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_DUPSORT) != 0;
}

/// \brief Checks whether the given value mode is the "ordinal" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "ordinal" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_ordinal(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_INTEGERDUP) != 0;
}

/// \brief Checks whether the given value mode is the "same-length" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "same-length" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_samelength(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_DUPFIXED) != 0;
}

/// \brief Checks whether the given value mode is the "reverse" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "reverse" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_reverse(value_mode mode) noexcept {
  return (MDBX_db_flags_t(mode) & MDBX_REVERSEDUP) != 0;
}

/// \brief Checks whether the given value mode is the "MessagePack" one.
/// \param mode  The value mode to be checked.
/// \returns `true` if the mode matches the "MessagePack" kind, `false` otherwise.
MDBX_CXX01_CONSTEXPR_ENUM bool is_msgpack(value_mode mode) noexcept { return mode == value_mode::msgpack; }

/// \brief A handle for an individual table (aka key-value space, maps or sub-database) in the environment.
/// \see txn::open_map() \see txn::create_map()
/// \see txn::clear_map() \see txn::drop_map()
/// \see txn::get_map_flags() \see txn::get_map_stat()
/// \see env::close_map()
/// \see cursor::map()
struct LIBMDBX_API_TYPE map_handle {
  MDBX_dbi dbi{0}; ///< \brief The table handle (DBI) number; zero for the main table.
  /// \brief Constructs an invalid (empty) handle.
  MDBX_CXX11_CONSTEXPR map_handle() noexcept {}
  /// \brief Constructs a handle from the given DBI number.
  /// \param dbi  The table handle (DBI) number.
  MDBX_CXX11_CONSTEXPR map_handle(MDBX_dbi dbi) noexcept : dbi(dbi) {}
  /// \brief Copy constructor.
  map_handle(const map_handle &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  map_handle &operator=(const map_handle &) noexcept = default;
  /// \brief Checks whether the handle is valid, i.e. not the main table.
  /// \returns `true` if the handle refers to a named table, `false` otherwise.
  operator bool() const noexcept { return dbi != 0; }
  /// \brief Returns the underlying DBI number.
  /// \returns The table handle (DBI) number.
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

  /// \brief An alias for \ref MDBX_db_flags_t, i.e. the table flags.
  using flags = ::MDBX_db_flags_t;
  /// \brief An alias for \ref MDBX_dbi_state_t, i.e. the table state.
  using state = ::MDBX_dbi_state_t;
  /// \brief Flags and state of a table (aka map).
  struct LIBMDBX_API_TYPE info {
    map_handle::flags flags; ///< \brief The table flags, see \ref MDBX_db_flags_t.
    map_handle::state state; ///< \brief The table state, see \ref MDBX_dbi_state_t.
    /// \brief Constructs the info from the given flags and state.
    /// \param flags  The table flags.
    /// \param state  The table state.
    MDBX_CXX11_CONSTEXPR info(map_handle::flags flags, map_handle::state state) noexcept;
    /// \brief Copy constructor.
    info(const info &) noexcept = default;
    /// \brief Copy assignment operator.
    /// \returns A reference to this instance.
    info &operator=(const info &) noexcept = default;
    /// \brief Returns the key comparison mode implied by the flags.
    /// \returns The \ref mdbx::key_mode of the table.
    MDBX_CXX11_CONSTEXPR_ENUM mdbx::key_mode key_mode() const noexcept;
    /// \brief Returns the value comparison mode implied by the flags.
    /// \returns The \ref mdbx::value_mode of the table.
    MDBX_CXX11_CONSTEXPR_ENUM mdbx::value_mode value_mode() const noexcept;
  };
};

/// \brief An alias for \ref MDBX_cmp_func, i.e. a comparison function pointer.
using comparator = ::MDBX_cmp_func;
/// \brief Returns the default internal key comparator for the given key mode.
/// \param mode  The key mode.
/// \returns The pointer to the default \ref comparator function.
inline comparator default_comparator(key_mode mode) noexcept {
  return ::mdbx_get_keycmp(static_cast<MDBX_db_flags_t>(mode));
}
/// \brief Returns the default internal value comparator for the given value mode.
/// \param mode  The value mode.
/// \returns The pointer to the default \ref comparator function.
inline comparator default_comparator(value_mode mode) noexcept {
  return ::mdbx_get_datacmp(static_cast<MDBX_db_flags_t>(mode));
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
