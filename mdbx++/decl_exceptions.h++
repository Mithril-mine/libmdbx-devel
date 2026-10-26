// > dist-cutoff-begin
#pragma once
#include "begin.h++"
namespace mdbx {
// < dist-cutoff-end

/// \defgroup cxx_exceptions Exceptions and Errors
/// @{

/// \brief Transfers C++ exceptions thru C callbacks.
/// \details Implements saving exceptions before returning
/// from an C++'s environment to the intermediate C code and re-throwing after
/// returning from C to the C++'s environment.
class LIBMDBX_API_TYPE exception_thunk {
  ::std::exception_ptr captured_;

public:
  /// \brief Default constructor, creates an empty (clean) thunk.
  exception_thunk() noexcept = default;
  /// \brief Copy construction is prohibited.
  exception_thunk(const exception_thunk &) = delete;
  /// \brief Move construction is prohibited.
  exception_thunk(exception_thunk &&) = delete;
  /// \brief Copy assignment is prohibited.
  exception_thunk &operator=(const exception_thunk &) = delete;
  /// \brief Move assignment is prohibited.
  exception_thunk &operator=(exception_thunk &&) = delete;
  /// \brief Checks whether no exception is captured.
  /// \returns `true` if the thunk is empty, `false` otherwise.
  inline bool is_clean() const noexcept;
  /// \brief Captures the currently thrown exception, if any.
  inline void capture() noexcept;
  /// \brief Re-throws the captured exception, if any.
  /// \throws The captured exception.
  inline void rethrow_captured() const;
};

/// \brief Implements error information and throwing corresponding exceptions.
class LIBMDBX_API_TYPE error {
  MDBX_error_t code_;
  /// \brief Assigns the given error code to this instance.
  /// \param error_code  The error code to be assigned.
  /// \returns A reference to this instance.
  inline error &operator=(MDBX_error_t error_code) noexcept;

public:
  /// \brief Constructs an error instance from the given code.
  /// \param error_code  The error code.
  MDBX_CXX11_CONSTEXPR error(MDBX_error_t error_code) noexcept;
  /// \brief Copy constructor.
  error(const error &) = default;
  /// \brief Move constructor.
  error(error &&) = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  error &operator=(const error &) = default;
  /// \brief Move assignment operator.
  /// \returns A reference to this instance.
  error &operator=(error &&) = default;

  /// \brief Compares two error codes for equality.
  /// \param a  The left-hand error.
  /// \param b  The right-hand error.
  /// \returns `true` if the error codes are equal, `false` otherwise.
  MDBX_CXX11_CONSTEXPR friend bool operator==(const error &a, const error &b) noexcept;
  /// \brief Compares two error codes for inequality.
  /// \param a  The left-hand error.
  /// \param b  The right-hand error.
  /// \returns `true` if the error codes are not equal, `false` otherwise.
  MDBX_CXX11_CONSTEXPR friend bool operator!=(const error &a, const error &b) noexcept;

  /// \brief Checks the error is a success.
  /// \returns `true` if the error code is \ref MDBX_SUCCESS, `false` otherwise.
  MDBX_CXX11_CONSTEXPR bool is_success() const noexcept;
  /// \brief Checks the error is the "true" result of a \ref MDBX_RESULT_TRUE.
  /// \returns `true` if the error code is \ref MDBX_RESULT_TRUE, `false` otherwise.
  MDBX_CXX11_CONSTEXPR bool is_result_true() const noexcept;
  /// \brief Checks the error is the "false" result of a \ref MDBX_RESULT_FALSE.
  /// \returns `true` if the error code is \ref MDBX_RESULT_FALSE, `false` otherwise.
  MDBX_CXX11_CONSTEXPR bool is_result_false() const noexcept;
  /// \brief Checks the error is a failure.
  /// \returns `true` if the error code is neither a success nor a boolean
  ///          result, `false` otherwise.
  MDBX_CXX11_CONSTEXPR bool is_failure() const noexcept;

  /// \brief Returns error code.
  /// \returns The stored \ref MDBX_error_t code.
  MDBX_CXX11_CONSTEXPR MDBX_error_t code() const noexcept;

  /// \brief Returns message for MDBX's errors only and "SYSTEM" for others.
  /// \returns The pointer to a constant string with the error message.
  const char *what() const noexcept;

  /// \brief Returns message for any errors.
  /// \returns The error message as a `std::string`.
  ::std::string message() const;

  /// \brief Returns true for MDBX's errors.
  /// \returns `true` if the error code belongs to libmdbx, `false` otherwise.
  MDBX_CXX11_CONSTEXPR bool is_mdbx_error() const noexcept;
  /// \brief Panics on unrecoverable errors inside destructors etc.
  /// \throws The corresponding \ref mdbx::exception if the error is a failure.
  [[noreturn]] void throw_exception() const;
  /// \brief Panics on unrecoverable errors inside destructors etc.
  /// \param error_code  The error code to be checked.
  /// \throws The corresponding \ref mdbx::exception if the error is a failure.
  [[noreturn]] static inline void throw_exception(int error_code);
  /// \brief Throws the corresponding exception if this error is a failure.
  /// \throws The corresponding \ref mdbx::exception.
  inline void throw_on_failure() const;
  /// \brief Throws the corresponding exception if this error is a failure.
  /// \throws The corresponding \ref mdbx::exception.
  inline void success_or_throw() const;
  /// \brief Throws the corresponding exception, or the captured one if any.
  /// \param thunk  The \ref mdbx::exception_thunk with a captured exception.
  /// \throws The corresponding \ref mdbx::exception.
  inline void success_or_throw(const exception_thunk &thunk) const;
  /// \brief Throws the corresponding exception if the pointer is null.
  /// \param ptr         The pointer to be checked.
  /// \param error_code  The error code to be thrown on a null pointer.
  /// \throws The corresponding \ref mdbx::exception.
  static inline void throw_on_nullptr(const void *ptr, MDBX_error_t error_code);
  /// \brief Throws the corresponding exception if the code is a failure.
  /// \param error_code  The error code to be checked.
  /// \throws The corresponding \ref mdbx::exception.
  static inline void success_or_throw(MDBX_error_t error_code);
  /// \brief Throws the corresponding exception if the code is a failure.
  /// \param error_code  The error code to be checked.
  /// \throws The corresponding \ref mdbx::exception.
  static void success_or_throw(int error_code) { success_or_throw(static_cast<MDBX_error_t>(error_code)); }
  /// \brief Throws the corresponding exception if the code is a failure.
  /// \param error_code  The error code to be checked.
  /// \throws The corresponding \ref mdbx::exception.
  static inline void throw_on_failure(int error_code);
  /// \brief Throws the corresponding exception if the code is a failure.
  /// \param error_code  The error code to be checked.
  /// \returns `true` if the code is \ref MDBX_RESULT_TRUE, `false` otherwise.
  /// \throws The corresponding \ref mdbx::exception.
  static inline bool boolean_or_throw(int error_code);
  /// \brief Throws the corresponding exception, or the captured one if any.
  /// \param error_code  The error code to be checked.
  /// \param thunk       The \ref mdbx::exception_thunk with a captured exception.
  /// \throws The corresponding \ref mdbx::exception.
  static inline void success_or_throw(int error_code, const exception_thunk &thunk);
  /// \brief Throws the corresponding exception, or the captured one if any.
  /// \param error_code  The error code to be checked.
  /// \param thunk       The \ref mdbx::exception_thunk with a captured exception.
  /// \returns `true` if the code is \ref MDBX_RESULT_TRUE, `false` otherwise.
  /// \throws The corresponding \ref mdbx::exception.
  static inline bool boolean_or_throw(int error_code, const exception_thunk &thunk);
};

/// \brief Base class for all libmdbx's exceptions that correspond to libmdbx errors.
/// \see MDBX_error_t
class LIBMDBX_API_TYPE exception : public ::std::runtime_error {
  using base = ::std::runtime_error;
  ::mdbx::error error_;

public:
  /// \brief Constructs the exception from the given error.
  /// \param err  The error information.
  exception(const ::mdbx::error &err);
  /// \brief Copy constructor.
  exception(const exception &) = default;
  /// \brief Move constructor.
  exception(exception &&) = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  exception &operator=(const exception &) = default;
  /// \brief Move assignment operator.
  /// \returns A reference to this instance.
  exception &operator=(exception &&) = default;
  /// \brief Destroys the exception.
  virtual ~exception() noexcept;
  /// \brief Returns the error information.
  /// \returns The stored \ref mdbx::error instance.
  const ::mdbx::error error() const noexcept { return error_; }
};

/// \brief Fatal exception that leads to termination anyway in dangerous unrecoverable cases.
class LIBMDBX_API_TYPE fatal : public exception {
  using base = exception;

public:
  /// \brief Constructs the fatal exception from the given error.
  /// \param err  The error information.
  fatal(const ::mdbx::error &err) noexcept;
  /// \brief Constructs the fatal exception from the given exception.
  /// \param src  The source exception.
  fatal(const exception &src) noexcept : fatal(src.error()) {}
  /// \brief Constructs the fatal exception from the given exception.
  /// \param src  The source exception.
  fatal(exception &&src) noexcept : fatal(src.error()) {}
  /// \brief Copy constructor.
  /// \param src  The source exception.
  fatal(const fatal &src) noexcept : fatal(src.error()) {}
  /// \brief Move constructor.
  /// \param src  The source exception.
  fatal(fatal &&src) noexcept : fatal(src.error()) {}
  /// \brief Move assignment operator.
  /// \returns A reference to this instance.
  fatal &operator=(fatal &&) = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  fatal &operator=(const fatal &) = default;
  /// \brief Destroys the fatal exception.
  virtual ~fatal() noexcept;
};

#define MDBX_DECLARE_EXCEPTION(NAME)                                                                                   \
  struct LIBMDBX_API_TYPE NAME : public exception {                                                                    \
    /** \brief Constructs the exception from the given \ref mdbx::error. */                                            \
    NAME(const ::mdbx::error &);                                                                                       \
    /** \brief Destroys the exception. */                                                                              \
    virtual ~NAME() noexcept;                                                                                          \
  }
/// \brief Thrown on \ref MDBX_BAD_DBI, i.e. an invalid table handle was used.
MDBX_DECLARE_EXCEPTION(bad_map_id);
/// \brief Thrown on \ref MDBX_BAD_TXN, i.e. a transaction handle is invalid or
/// was used after being finished.
MDBX_DECLARE_EXCEPTION(bad_transaction);
/// \brief Thrown on \ref MDBX_BAD_VALSIZE, i.e. an unsupported size of a key
/// or a data item.
MDBX_DECLARE_EXCEPTION(bad_value_size);
/// \brief Thrown on \ref MDBX_CORRUPTED, i.e. the database is corrupted or an
/// invalid data was detected.
MDBX_DECLARE_EXCEPTION(db_corrupted);
/// \brief Thrown on \ref MDBX_MAP_FULL, i.e. the database is full and the
/// environment cannot be extended.
MDBX_DECLARE_EXCEPTION(db_full);
/// \brief Thrown on \ref MDBX_INVALID, i.e. an invalid parameter was specified
/// or the environment is already closed.
MDBX_DECLARE_EXCEPTION(db_invalid);
/// \brief Thrown on \ref MDBX_TOO_LARGE, i.e. the database is too large for
/// the current process/environment.
MDBX_DECLARE_EXCEPTION(db_too_large);
/// \brief Thrown on \ref MDBX_UNABLE_EXTEND_MAPSIZE, i.e. the requested
/// geometry change needs the mapping to be re-created but it could not be
/// applied right now.
MDBX_DECLARE_EXCEPTION(db_unable_extend);
/// \brief Thrown on \ref MDBX_VERSION_MISMATCH, i.e. the database was created
/// by an incompatible version of libmdbx.
MDBX_DECLARE_EXCEPTION(db_version_mismatch);
/// \brief Thrown on \ref MDBX_WANNA_RECOVERY, i.e. the database requires
/// recovery using `mdbx_chk` and/or `mdbx_env_open_for_recovery()`.
MDBX_DECLARE_EXCEPTION(db_wanna_write_for_recovery);
/// \brief Thrown on \ref MDBX_INCOMPATIBLE, i.e. an operation is incompatible
/// with the environment/table options.
MDBX_DECLARE_EXCEPTION(incompatible_operation);
/// \brief Thrown on \ref MDBX_PAGE_FULL, i.e. an item does not fit into a page
/// of the table.
MDBX_DECLARE_EXCEPTION(internal_page_full);
/// \brief Thrown on \ref MDBX_PROBLEM, i.e. an unexpected internal problem
/// was detected.
MDBX_DECLARE_EXCEPTION(internal_problem);
/// \brief Thrown on \ref MDBX_KEYEXIST, i.e. the key/data pair already exists
/// and \ref MDBX_NOOVERWRITE was specified.
MDBX_DECLARE_EXCEPTION(key_exists);
/// \brief Thrown on \ref MDBX_EKEYMISMATCH, i.e. an insert with the append
/// hint but the key is not the last one.
MDBX_DECLARE_EXCEPTION(key_mismatch);
/// \brief Thrown on \ref MDBX_DBS_FULL, i.e. the maximum number of table
/// handles has been reached.
MDBX_DECLARE_EXCEPTION(max_maps_reached);
/// \brief Thrown on \ref MDBX_READERS_FULL, i.e. the environment's reader
/// table is full.
MDBX_DECLARE_EXCEPTION(max_readers_reached);
/// \brief Thrown on \ref MDBX_EMULTIVAL, i.e. the operation is not supported
/// for the multi-value (dupsort) key.
MDBX_DECLARE_EXCEPTION(multivalue);
/// \brief Thrown on \ref MDBX_ENODATA, i.e. the requested data is missing.
MDBX_DECLARE_EXCEPTION(no_data);
/// \brief Thrown on \ref MDBX_NOTFOUND, i.e. the requested key/data pair was
/// not found.
MDBX_DECLARE_EXCEPTION(not_found);
/// \brief Thrown on \ref MDBX_EPERM, i.e. the operation is not permitted in
/// the current state/environment mode.
MDBX_DECLARE_EXCEPTION(operation_not_permitted);
/// \brief Thrown on \ref MDBX_EACCESS, i.e. a permission was denied or the
/// environment/data is not writable.
MDBX_DECLARE_EXCEPTION(permission_denied_or_not_writeable);
/// \brief Thrown on \ref MDBX_BAD_RSLOT, i.e. a reader slot is invalid or busy.
MDBX_DECLARE_EXCEPTION(reader_slot_busy);
/// \brief Thrown on \ref MDBX_EREMOTE, i.e. a file/locking system call failed
/// for the reason that could not be diagnosed.
MDBX_DECLARE_EXCEPTION(remote_media);
/// \brief Thrown on \ref MDBX_BUSY, i.e. some resource is busy by another
/// thread or process.
MDBX_DECLARE_EXCEPTION(something_busy);
/// \brief Thrown on \ref MDBX_THREAD_MISMATCH, i.e. the transaction/cursor is
/// used by an alien thread.
MDBX_DECLARE_EXCEPTION(thread_mismatch);
/// \brief Thrown on \ref MDBX_TXN_FULL, i.e. the transaction has too many
/// dirty pages and cannot be continued.
MDBX_DECLARE_EXCEPTION(transaction_full);
/// \brief Thrown on \ref MDBX_TXN_OVERLAPPING, i.e. a write transaction was
/// started while another one is in progress by the same process.
MDBX_DECLARE_EXCEPTION(transaction_overlapping);
/// \brief Thrown on \ref MDBX_DUPLICATED_LCK, i.e. the lock file was created in
/// a manner that conflicts with the current process' state.
MDBX_DECLARE_EXCEPTION(duplicated_lck_file);
/// \brief Thrown on \ref MDBX_DANGLING_DBI, i.e. a table handle refers to
/// a non-existent table.
MDBX_DECLARE_EXCEPTION(dangling_map_id);
/// \brief Thrown on \ref MDBX_OUSTED, i.e. the current write transaction was
/// ousted by a newer one.
MDBX_DECLARE_EXCEPTION(transaction_ousted);
/// \brief Thrown on \ref MDBX_MVCC_RETARDED, i.e. the MVCC snapshot is
/// retarded too much to continue.
MDBX_DECLARE_EXCEPTION(mvcc_retarded);
/// \brief Thrown on \ref MDBX_LAGGARD_READER, i.e. the reader is lagging
/// too much behind.
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

/// \brief The only length allowed for a null pointer: zero.
/// A null pointer with a non-zero length would dereference address zero;
/// it is rejected with \ref bad_value_size.
MDBX_MAYBE_UNUSED static MDBX_CXX14_CONSTEXPR size_t check_nullptr_length(size_t bytes) {
  if (MDBX_UNLIKELY(bytes != 0))
    MDBX_CXX20_UNLIKELY throw_bad_value_size();
  return bytes;
}

/// end of cxx_exceptions @}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
