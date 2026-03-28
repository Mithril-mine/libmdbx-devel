// > dist-cutoff-begin
#pragma once
#include "decl_exceptions.h++"
namespace mdbx {
// < dist-cutoff-end

inline bool exception_thunk::is_clean() const noexcept { return !captured_; }

inline void exception_thunk::capture() noexcept {
  assert(is_clean());
  captured_ = ::std::current_exception();
}

inline void exception_thunk::rethrow_captured() const {
  if (captured_)
    MDBX_CXX20_UNLIKELY ::std::rethrow_exception(captured_);
}

//------------------------------------------------------------------------------

MDBX_CXX11_CONSTEXPR error::error(MDBX_error_t error_code) noexcept : code_(error_code) {}

inline error &error::operator=(MDBX_error_t error_code) noexcept {
  code_ = error_code;
  return *this;
}

MDBX_CXX11_CONSTEXPR bool operator==(const error &a, const error &b) noexcept { return a.code_ == b.code_; }

MDBX_CXX11_CONSTEXPR bool operator!=(const error &a, const error &b) noexcept { return !(a == b); }

MDBX_CXX11_CONSTEXPR bool error::is_success() const noexcept { return code_ == MDBX_SUCCESS; }

MDBX_CXX11_CONSTEXPR bool error::is_result_true() const noexcept { return code_ == MDBX_RESULT_FALSE; }

MDBX_CXX11_CONSTEXPR bool error::is_result_false() const noexcept { return code_ == MDBX_RESULT_TRUE; }

MDBX_CXX11_CONSTEXPR bool error::is_failure() const noexcept {
  return code_ != MDBX_SUCCESS && code_ != MDBX_RESULT_TRUE;
}

MDBX_CXX11_CONSTEXPR MDBX_error_t error::code() const noexcept { return code_; }

MDBX_CXX11_CONSTEXPR bool error::is_mdbx_error() const noexcept {
  return (code() >= MDBX_FIRST_LMDB_ERRCODE && code() <= MDBX_LAST_LMDB_ERRCODE) ||
         (code() >= MDBX_FIRST_ADDED_ERRCODE && code() <= MDBX_LAST_ADDED_ERRCODE);
}

inline void error::throw_exception(int error_code) {
  const error trouble(static_cast<MDBX_error_t>(error_code));
  trouble.throw_exception();
}

inline void error::throw_on_failure() const {
  if (MDBX_UNLIKELY(is_failure()))
    MDBX_CXX20_UNLIKELY throw_exception();
}

inline void error::success_or_throw() const {
  if (MDBX_UNLIKELY(!is_success()))
    MDBX_CXX20_UNLIKELY throw_exception();
}

inline void error::success_or_throw(const exception_thunk &thunk) const {
  assert(thunk.is_clean() || code() != MDBX_SUCCESS);
  if (MDBX_UNLIKELY(!is_success())) {
    MDBX_CXX20_UNLIKELY if (MDBX_UNLIKELY(!thunk.is_clean())) thunk.rethrow_captured();
    else throw_exception();
  }
}

inline void error::throw_on_nullptr(const void *ptr, MDBX_error_t error_code) {
  if (MDBX_UNLIKELY(ptr == nullptr))
    MDBX_CXX20_UNLIKELY error(error_code).throw_exception();
}

inline void error::throw_on_failure(int error_code) {
  error rc(static_cast<MDBX_error_t>(error_code));
  rc.throw_on_failure();
}

inline void error::success_or_throw(MDBX_error_t error_code) {
  error rc(error_code);
  rc.success_or_throw();
}

inline bool error::boolean_or_throw(int error_code) {
  switch (error_code) {
  case MDBX_RESULT_FALSE:
    return false;
  case MDBX_RESULT_TRUE:
    return true;
  default:
    MDBX_CXX20_UNLIKELY throw_exception(error_code);
  }
}

inline void error::success_or_throw(int error_code, const exception_thunk &thunk) {
  error rc(static_cast<MDBX_error_t>(error_code));
  rc.success_or_throw(thunk);
}

inline bool error::boolean_or_throw(int error_code, const exception_thunk &thunk) {
  if (MDBX_UNLIKELY(!thunk.is_clean()))
    MDBX_CXX20_UNLIKELY thunk.rethrow_captured();
  return boolean_or_throw(error_code);
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
