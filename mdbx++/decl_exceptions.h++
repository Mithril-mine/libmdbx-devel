// > dist-cutoff-begin
#pragma once
#include "begin.h++"
namespace mdbx {
// < dist-cutoff-end

/// \defgroup cxx_exceptions exceptions and errors
/// @{

/// \brief Transfers C++ exceptions thru C callbacks.
/// \details Implements saving exceptions before returning
/// from an C++'s environment to the intermediate C code and re-throwing after
/// returning from C to the C++'s environment.
class LIBMDBX_API_TYPE exception_thunk {
  ::std::exception_ptr captured_;

public:
  exception_thunk() noexcept = default;
  exception_thunk(const exception_thunk &) = delete;
  exception_thunk(exception_thunk &&) = delete;
  exception_thunk &operator=(const exception_thunk &) = delete;
  exception_thunk &operator=(exception_thunk &&) = delete;
  inline bool is_clean() const noexcept;
  inline void capture() noexcept;
  inline void rethrow_captured() const;
};

/// \brief Implements error information and throwing corresponding exceptions.
class LIBMDBX_API_TYPE error {
  MDBX_error_t code_;
  inline error &operator=(MDBX_error_t error_code) noexcept;

public:
  MDBX_CXX11_CONSTEXPR error(MDBX_error_t error_code) noexcept;
  error(const error &) = default;
  error(error &&) = default;
  error &operator=(const error &) = default;
  error &operator=(error &&) = default;

  MDBX_CXX11_CONSTEXPR friend bool operator==(const error &a, const error &b) noexcept;
  MDBX_CXX11_CONSTEXPR friend bool operator!=(const error &a, const error &b) noexcept;

  MDBX_CXX11_CONSTEXPR bool is_success() const noexcept;
  MDBX_CXX11_CONSTEXPR bool is_result_true() const noexcept;
  MDBX_CXX11_CONSTEXPR bool is_result_false() const noexcept;
  MDBX_CXX11_CONSTEXPR bool is_failure() const noexcept;

  /// \brief Returns error code.
  MDBX_CXX11_CONSTEXPR MDBX_error_t code() const noexcept;

  /// \brief Returns message for MDBX's errors only and "SYSTEM" for others.
  const char *what() const noexcept;

  /// \brief Returns message for any errors.
  ::std::string message() const;

  /// \brief Returns true for MDBX's errors.
  MDBX_CXX11_CONSTEXPR bool is_mdbx_error() const noexcept;
  /// \brief Panics on unrecoverable errors inside destructors etc.
  [[noreturn]] void panic(const char *context_where_when, const char *func_who_what) const noexcept;
  [[noreturn]] void throw_exception() const;
  [[noreturn]] static inline void throw_exception(int error_code);
  inline void throw_on_failure() const;
  inline void success_or_throw() const;
  inline void success_or_throw(const exception_thunk &) const;
  inline void panic_on_failure(const char *context_where, const char *func_who) const noexcept;
  inline void success_or_panic(const char *context_where, const char *func_who) const noexcept;
  static inline void throw_on_nullptr(const void *ptr, MDBX_error_t error_code);
  static inline void success_or_throw(MDBX_error_t error_code);
  static void success_or_throw(int error_code) { success_or_throw(static_cast<MDBX_error_t>(error_code)); }
  static inline void throw_on_failure(int error_code);
  static inline bool boolean_or_throw(int error_code);
  static inline void success_or_throw(int error_code, const exception_thunk &);
  static inline bool boolean_or_throw(int error_code, const exception_thunk &);
  static inline void panic_on_failure(int error_code, const char *context_where, const char *func_who) noexcept;
  static inline void success_or_panic(int error_code, const char *context_where, const char *func_who) noexcept;
};

/// \brief Base class for all libmdbx's exceptions that are corresponds to libmdbx errors.
/// \see MDBX_error_t
class LIBMDBX_API_TYPE exception : public ::std::runtime_error {
  using base = ::std::runtime_error;
  ::mdbx::error error_;

public:
  exception(const ::mdbx::error &) noexcept;
  exception(const exception &) = default;
  exception(exception &&) = default;
  exception &operator=(const exception &) = default;
  exception &operator=(exception &&) = default;
  virtual ~exception() noexcept;
  const ::mdbx::error error() const noexcept { return error_; }
};

/// \brief Fatal exception that lead termination anyway in dangerous unrecoverable cases.
class LIBMDBX_API_TYPE fatal : public exception {
  using base = exception;

public:
  fatal(const ::mdbx::error &) noexcept;
  fatal(const exception &src) noexcept : fatal(src.error()) {}
  fatal(exception &&src) noexcept : fatal(src.error()) {}
  fatal(const fatal &src) noexcept : fatal(src.error()) {}
  fatal(fatal &&src) noexcept : fatal(src.error()) {}
  fatal &operator=(fatal &&) = default;
  fatal &operator=(const fatal &) = default;
  virtual ~fatal() noexcept;
};

#define MDBX_DECLARE_EXCEPTION(NAME)                                                                                   \
  struct LIBMDBX_API_TYPE NAME : public exception {                                                                    \
    NAME(const ::mdbx::error &);                                                                                       \
    virtual ~NAME() noexcept;                                                                                          \
  }
MDBX_DECLARE_EXCEPTION(bad_map_id);
MDBX_DECLARE_EXCEPTION(bad_transaction);
MDBX_DECLARE_EXCEPTION(bad_value_size);
MDBX_DECLARE_EXCEPTION(db_corrupted);
MDBX_DECLARE_EXCEPTION(db_full);
MDBX_DECLARE_EXCEPTION(db_invalid);
MDBX_DECLARE_EXCEPTION(db_too_large);
MDBX_DECLARE_EXCEPTION(db_unable_extend);
MDBX_DECLARE_EXCEPTION(db_version_mismatch);
MDBX_DECLARE_EXCEPTION(db_wanna_write_for_recovery);
MDBX_DECLARE_EXCEPTION(incompatible_operation);
MDBX_DECLARE_EXCEPTION(internal_page_full);
MDBX_DECLARE_EXCEPTION(internal_problem);
MDBX_DECLARE_EXCEPTION(key_exists);
MDBX_DECLARE_EXCEPTION(key_mismatch);
MDBX_DECLARE_EXCEPTION(max_maps_reached);
MDBX_DECLARE_EXCEPTION(max_readers_reached);
MDBX_DECLARE_EXCEPTION(multivalue);
MDBX_DECLARE_EXCEPTION(no_data);
MDBX_DECLARE_EXCEPTION(not_found);
MDBX_DECLARE_EXCEPTION(operation_not_permitted);
MDBX_DECLARE_EXCEPTION(permission_denied_or_not_writeable);
MDBX_DECLARE_EXCEPTION(reader_slot_busy);
MDBX_DECLARE_EXCEPTION(remote_media);
MDBX_DECLARE_EXCEPTION(something_busy);
MDBX_DECLARE_EXCEPTION(thread_mismatch);
MDBX_DECLARE_EXCEPTION(transaction_full);
MDBX_DECLARE_EXCEPTION(transaction_overlapping);
MDBX_DECLARE_EXCEPTION(duplicated_lck_file);
MDBX_DECLARE_EXCEPTION(dangling_map_id);
MDBX_DECLARE_EXCEPTION(transaction_ousted);
MDBX_DECLARE_EXCEPTION(mvcc_retarded);
MDBX_DECLARE_EXCEPTION(laggard_reader);
#undef MDBX_DECLARE_EXCEPTION

[[noreturn]] LIBMDBX_API void throw_too_small_target_buffer();
[[noreturn]] LIBMDBX_API void throw_max_length_exceeded();
[[noreturn]] LIBMDBX_API void throw_out_range();
[[noreturn]] LIBMDBX_API void throw_allocators_mismatch();
[[noreturn]] LIBMDBX_API void throw_bad_value_size();
[[noreturn]] LIBMDBX_API void throw_incomparable_cursors();

static MDBX_CXX14_CONSTEXPR size_t check_length(size_t bytes) {
  if (MDBX_UNLIKELY(bytes > size_t(MDBX_MAXDATASIZE)))
    MDBX_CXX20_UNLIKELY throw_max_length_exceeded();
  return bytes;
}

static MDBX_CXX14_CONSTEXPR size_t check_length(size_t headroom, size_t payload) {
  return check_length(check_length(headroom) + check_length(payload));
}

MDBX_MAYBE_UNUSED static MDBX_CXX14_CONSTEXPR size_t check_length(size_t headroom, size_t payload, size_t tailroom) {
  return check_length(check_length(headroom, payload) + check_length(tailroom));
}

/// end of cxx_exceptions @}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
