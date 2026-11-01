// > dist-cutoff-begin
#pragma once
#include "decl-core.h++"
#include "decl-cursor.h++"
#include "decl-env.h++"
#include "decl-txn.h++"
namespace mdbx {
// < dist-cutoff-end

LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const slice &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const pair &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const pair_result &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::geometry::size &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::geometry &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::operate_parameters &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::mode &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::durability &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::reclaiming_options &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env::operate_options &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const env_managed::create_parameters &);

LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const MDBX_log_level_t &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const MDBX_debug_flags_t &);
LIBMDBX_API ::std::ostream &operator<<(::std::ostream &, const error &);

inline ::std::ostream &operator<<(::std::ostream &out, const MDBX_error_t &errcode) { return out << error(errcode); }

/// end cxx_api @}

} // namespace mdbx

//------------------------------------------------------------------------------

/// \brief The `std` namespace part of libmdbx C++ API
/// \ingroup cxx_api
namespace std {

/// \brief Converts the given \ref mdbx::slice to a string.
/// \param value  The slice to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::slice &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::buffer to a string.
/// \param buffer  The buffer to be converted.
/// \returns The string representation.
template <class ALLOCATOR, typename CAPACITY_POLICY>
inline string to_string(const ::mdbx::buffer<ALLOCATOR, CAPACITY_POLICY> &buffer) {
  ostringstream out;
  out << buffer;
  return out.str();
}

/// \brief Converts the given \ref mdbx::pair to a string.
/// \param value  The key-value pair to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::pair &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::geometry to a string.
/// \param value  The geometry to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::geometry &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::operate_parameters to a string.
/// \param value  The operate parameters to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::operate_parameters &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::mode to a string.
/// \param value  The mode to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::mode &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::durability to a string.
/// \param value  The durability to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::durability &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::reclaiming_options to a string.
/// \param value  The reclaiming options to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::reclaiming_options &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env::operate_options to a string.
/// \param value  The operate options to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env::operate_options &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::env_managed::create_parameters to a string.
/// \param value  The create parameters to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::env_managed::create_parameters &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref MDBX_log_level_t to a string.
/// \param value  The log level to be converted.
/// \returns The string representation.
inline string to_string(const ::MDBX_log_level_t &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref MDBX_debug_flags_t to a string.
/// \param value  The debug flags to be converted.
/// \returns The string representation.
inline string to_string(const ::MDBX_debug_flags_t &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref mdbx::error to a string.
/// \param value  The error to be converted.
/// \returns The string representation.
inline string to_string(const ::mdbx::error &value) {
  ostringstream out;
  out << value;
  return out.str();
}

/// \brief Converts the given \ref MDBX_error_t code to a string.
/// \param errcode  The error code to be converted.
/// \returns The string representation.
inline string to_string(const ::MDBX_error_t &errcode) { return to_string(::mdbx::error(errcode)); }

template <> struct hash<::mdbx::slice> {
  MDBX_CXX14_CONSTEXPR size_t operator()(const ::mdbx::slice &slice) const noexcept { return slice.hash_value(); }
};

template <> struct hash<::mdbx::map_handle> {
  MDBX_CXX11_CONSTEXPR size_t operator()(const ::mdbx::map_handle &handle) const noexcept { return handle.dbi; }
};

template <> struct hash<::mdbx::env> : public std::hash<const MDBX_env *> {
  using inherited = std::hash<const MDBX_env *>;
  size_t operator()(const ::mdbx::env &env) const noexcept { return inherited::operator()(env); }
};

template <> struct hash<::mdbx::txn> : public std::hash<const MDBX_txn *> {
  using inherited = std::hash<const MDBX_txn *>;
  size_t operator()(const ::mdbx::txn &txn) const noexcept { return inherited::operator()(txn); }
};

template <> struct hash<::mdbx::cursor> : public std::hash<const MDBX_cursor *> {
  using inherited = std::hash<const MDBX_cursor *>;
  size_t operator()(const ::mdbx::cursor &cursor) const noexcept { return inherited::operator()(cursor); }
};

template <class ALLOCATOR, typename CAPACITY_POLICY> struct hash<::mdbx::buffer<ALLOCATOR, CAPACITY_POLICY>> {
  size_t operator()(::mdbx::buffer<ALLOCATOR, CAPACITY_POLICY> const &buffer) const noexcept {
    return buffer.hash_value();
  }
};

} // namespace std

#if defined(__LCC__) && __LCC__ >= 126
#pragma diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif
