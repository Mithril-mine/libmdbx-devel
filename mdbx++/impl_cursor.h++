// > dist-cutoff-begin
#pragma once
#include "decl_cursor.h++"
#include "decl_env.h++"
#include "decl_txn.h++"
namespace mdbx {
// < dist-cutoff-end

MDBX_CXX11_CONSTEXPR cursor::cursor(MDBX_cursor *ptr) noexcept : handle_(ptr) {}

inline cursor &cursor::assign(const cursor &src) {
  error::success_or_throw(::mdbx_cursor_copy(src.handle_, handle_));
  return *this;
}

inline cursor &cursor::operator=(cursor &&other) noexcept {
  if (this != &other) {
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

inline cursor::cursor(cursor &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

inline cursor::~cursor() noexcept {
#if (defined(MDBX_CHECKING) && MDBX_CHECKING > 0) || (defined(MDBX_DEBUG) && MDBX_DEBUG > 0)
  handle_ = reinterpret_cast<MDBX_cursor *>(uintptr_t(0xDeadBeef));
#endif
}

inline void *cursor::get_context() const noexcept { return mdbx_cursor_get_userctx(handle_); }

inline cursor &cursor::set_context(void *ptr) {
  error::success_or_throw(::mdbx_cursor_set_userctx(handle_, ptr));
  return *this;
}

inline int compare_position_nothrow(const cursor &left, const cursor &right, bool ignore_nested = false) noexcept {
  return mdbx_cursor_compare(left.handle_, right.handle_, ignore_nested);
}

inline int compare_position(const cursor &left, const cursor &right, bool ignore_nested = false) {
  const auto diff = compare_position_nothrow(left, right, ignore_nested);
  MDBX_INLINE_API_ASSERT(compare_position_nothrow(right, left, ignore_nested) == -diff);
  if (MDBX_LIKELY(int16_t(diff) == diff))
    MDBX_CXX20_LIKELY return int(diff);
  else
    throw_incomparable_cursors();
}

inline cursor::move_result::move_result(const cursor &cursor, bool throw_notfound) : pair_result() {
  done = cursor.move(get_current, &this->key, &this->value, throw_notfound);
}

inline cursor::move_result::move_result(cursor &cursor, move_operation operation, const slice &key, const slice &value,
                                        bool throw_notfound)
    : pair_result(key, value, false) {
  this->done = cursor.move(operation, &this->key, &this->value, throw_notfound);
}

inline bool cursor::move(move_operation operation, MDBX_val *key, MDBX_val *value, bool throw_notfound) const {
  const int err = ::mdbx_cursor_get(handle_, key, value, MDBX_cursor_op(operation));
  switch (err) {
  case MDBX_SUCCESS:
    MDBX_CXX20_LIKELY return true;
  case MDBX_RESULT_TRUE:
    return false;
  case MDBX_NOTFOUND:
    if (!throw_notfound)
      return false;
    MDBX_CXX17_FALLTHROUGH /* fallthrough */;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline cursor::estimate_result::estimate_result(const cursor &cursor, move_operation operation, const slice &key,
                                                const slice &value)
    : pair(key, value), approximate_quantity(PTRDIFF_MIN) {
  approximate_quantity = cursor.estimate(operation, &this->key, &this->value);
}

inline ptrdiff_t cursor::estimate(move_operation operation, MDBX_val *key, MDBX_val *value) const {
  ptrdiff_t result;
  error::success_or_throw(::mdbx_estimate_move(*this, key, value, MDBX_cursor_op(operation), &result));
  return result;
}

inline ptrdiff_t estimate(const cursor &from, const cursor &to) {
  ptrdiff_t result;
  error::success_or_throw(mdbx_estimate_distance(from, to, &result));
  return result;
}

inline cursor::move_result cursor::find(const slice &key, bool throw_notfound) {
  return move(key_exact, key, throw_notfound);
}

inline cursor::move_result cursor::lower_bound(const slice &key, bool throw_notfound) {
  return move(key_lowerbound, key, throw_notfound);
}

inline cursor::move_result cursor::upper_bound(const slice &key, bool throw_notfound) {
  return move(key_greater_than, key, throw_notfound);
}

inline cursor::move_result cursor::find_multivalue(const slice &key, const slice &value, bool throw_notfound) {
  return move(multi_find_pair, key, value, throw_notfound);
}

inline cursor::move_result cursor::lower_bound_multivalue(const slice &key, const slice &value, bool throw_notfound) {
  return move(multi_exactkey_lowerboundvalue, key, value, throw_notfound);
}

inline cursor::move_result cursor::upper_bound_multivalue(const slice &key, const slice &value, bool throw_notfound) {
  return move(multi_exactkey_value_greater, key, value, throw_notfound);
}

inline bool cursor::seek(const slice &key) { return move(seek_key, const_cast<slice *>(&key), nullptr, false); }

inline size_t cursor::count_multivalue() const {
  size_t result;
  error::success_or_throw(::mdbx_cursor_count(*this, &result));
  return result;
}

inline bool cursor::eof() const { return error::boolean_or_throw(::mdbx_cursor_eof(*this)); }

inline bool cursor::on_first() const { return error::boolean_or_throw(::mdbx_cursor_on_first(*this)); }

inline bool cursor::on_last() const { return error::boolean_or_throw(::mdbx_cursor_on_last(*this)); }

inline bool cursor::on_first_multival() const { return error::boolean_or_throw(::mdbx_cursor_on_first_dup(*this)); }

inline bool cursor::on_last_multival() const { return error::boolean_or_throw(::mdbx_cursor_on_last_dup(*this)); }

inline cursor::estimate_result cursor::estimate(const slice &key, const slice &value) const {
  return estimate_result(*this, multi_exactkey_lowerboundvalue, key, value);
}

inline cursor::estimate_result cursor::estimate(const slice &key) const {
  return estimate_result(*this, key_lowerbound, key);
}

inline cursor::estimate_result cursor::estimate(move_operation operation) const {
  return estimate_result(*this, operation);
}

inline cursor::estimate_result cursor::estimate(move_operation operation, const slice &key) const {
  return estimate_result(*this, operation, key);
}

inline void cursor::renew(::mdbx::txn &txn) { error::success_or_throw(::mdbx_cursor_renew(txn, handle_)); }

inline void cursor::bind(::mdbx::txn &txn, ::mdbx::map_handle map_handle) {
  error::success_or_throw(::mdbx_cursor_bind(txn, handle_, map_handle.dbi));
}

inline void cursor::unbind() { error::success_or_throw(::mdbx_cursor_unbind(handle_)); }

inline txn cursor::txn() const {
  MDBX_txn *txn = ::mdbx_cursor_txn(handle_);
  return ::mdbx::txn(txn);
}

inline map_handle cursor::map() const {
  const MDBX_dbi dbi = ::mdbx_cursor_dbi(handle_);
  if (MDBX_UNLIKELY(dbi > MDBX_MAX_DBI))
    error::throw_exception(MDBX_EINVAL);
  return map_handle(dbi);
}

inline MDBX_error_t cursor::put(const slice &key, slice *value, MDBX_put_flags_t flags) noexcept {
  return MDBX_error_t(::mdbx_cursor_put(handle_, &key, value, flags));
}

inline void cursor::put(const slice &key, slice value, put_mode mode) {
  error::success_or_throw(put(key, &value, MDBX_put_flags_t(mode)));
}

inline void cursor::insert(const slice &key, slice value) {
  error::success_or_throw(
      put(key, &value /* takes the present value in case MDBX_KEYEXIST */, MDBX_put_flags_t(put_mode::insert_unique)));
}

inline value_result cursor::try_insert(const slice &key, slice value) {
  const int err =
      put(key, &value /* takes the present value in case MDBX_KEYEXIST */, MDBX_put_flags_t(put_mode::insert_unique));
  switch (err) {
  case MDBX_SUCCESS:
    return value_result{slice(), true};
  case MDBX_KEYEXIST:
    return value_result{value, false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline slice cursor::insert_reserve(const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  error::success_or_throw(put(key, &result /* takes the present value in case MDBX_KEYEXIST */,
                              MDBX_put_flags_t(put_mode::insert_unique) | MDBX_RESERVE));
  return result;
}

inline value_result cursor::try_insert_reserve(const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  const int err = put(key, &result /* takes the present value in case MDBX_KEYEXIST */,
                      MDBX_put_flags_t(put_mode::insert_unique) | MDBX_RESERVE);
  switch (err) {
  case MDBX_SUCCESS:
    return value_result{result, true};
  case MDBX_KEYEXIST:
    return value_result{result, false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline void cursor::upsert(const slice &key, const slice &value) {
  error::success_or_throw(put(key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::upsert)));
}

inline slice cursor::upsert_reserve(const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  error::success_or_throw(put(key, &result, MDBX_put_flags_t(put_mode::upsert) | MDBX_RESERVE));
  return result;
}

inline void cursor::update(const slice &key, const slice &value) {
  error::success_or_throw(put(key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::update)));
}

inline bool cursor::try_update(const slice &key, const slice &value) {
  const int err = put(key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::update));
  switch (err) {
  case MDBX_SUCCESS:
    return true;
  case MDBX_NOTFOUND:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline slice cursor::update_reserve(const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  error::success_or_throw(put(key, &result, MDBX_put_flags_t(put_mode::update) | MDBX_RESERVE));
  return result;
}

inline value_result cursor::try_update_reserve(const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  const int err = put(key, &result, MDBX_put_flags_t(put_mode::update) | MDBX_RESERVE);
  switch (err) {
  case MDBX_SUCCESS:
    return value_result{result, true};
  case MDBX_NOTFOUND:
    return value_result{slice(), false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline bool cursor::erase(bool whole_multivalue) {
  const int err = ::mdbx_cursor_del(handle_, whole_multivalue ? MDBX_ALLDUPS : MDBX_CURRENT);
  switch (err) {
  case MDBX_SUCCESS:
    MDBX_CXX20_LIKELY return true;
  case MDBX_NOTFOUND:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline bool cursor::erase(const slice &key, bool whole_multivalue) {
  bool found = seek(key);
  return found ? erase(whole_multivalue) : found;
}

inline bool cursor::erase(const slice &key, const slice &value) {
  move_result data = find_multivalue(key, value, false);
  return data.done && erase();
}

inline size_t cursor::put_multiple_samelength(const slice &key, const size_t value_length, const void *values_array,
                                              size_t values_count, put_mode mode, bool allow_partial) {
  MDBX_val args[2] = {{const_cast<void *>(values_array), value_length}, {nullptr, values_count}};
  const int err = ::mdbx_cursor_put(handle_, const_cast<slice *>(&key), args, MDBX_put_flags_t(mode) | MDBX_MULTIPLE);
  switch (err) {
  case MDBX_SUCCESS:
    MDBX_CXX20_LIKELY break;
  case MDBX_KEYEXIST:
    if (allow_partial)
      break;
    mdbx_txn_break(txn());
    MDBX_CXX17_FALLTHROUGH /* fallthrough */;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
  return args[1].iov_len /* done item count */;
}

inline ptrdiff_t cursor::distance_between(const cursor from, const cursor to, unsigned deepness) {
  intptr_t distance = PTRDIFF_MIN;
  error::success_or_throw(mdbx_cursor_distance(from, to, &distance, deepness));
  return distance;
}

inline bool cursor::scroll(intptr_t distance, unsigned deepness, bool throw_notfound) {
  const int err = ::mdbx_cursor_scroll(handle_, distance, deepness);
  switch (err) {
  case MDBX_SUCCESS:
    MDBX_CXX20_LIKELY return true;
  case MDBX_NOTFOUND:
    if (!throw_notfound)
      return false;
    MDBX_CXX17_FALLTHROUGH /* fallthrough */;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline bool cursor::distribute(const cursor from, const cursor to, cursor *cursors_array, intptr_t cursors_array_size,
                               unsigned deepness) {
  static_assert(sizeof(cursors_array[0]) == sizeof(MDBX_cursor *), "oops");
  const int err = ::mdbx_cursor_distribute(from, to, &cursors_array[0].handle_, cursors_array_size, deepness);
  return error::boolean_or_throw(err);
}

inline bool cursor::distribute(const cursor from, const cursor to, const std::vector<cursor> &cursors_array,
                               unsigned deepness) {
  static_assert(sizeof(cursor) == sizeof(MDBX_cursor *), "oops");
  const int err = MDBX_LIKELY(!cursors_array.empty())
                      ? ::mdbx_cursor_distribute(from, to, const_cast<MDBX_cursor **>(&cursors_array[0].handle_),
                                                 cursors_array.size(), deepness)
                      : MDBX_EINVAL;
  return error::boolean_or_throw(err);
}

inline bool cursor::distribute(const cursor from, const cursor to, const std::vector<cursor_managed> &cursors_array,
                               unsigned deepness) {
  static_assert(sizeof(cursor_managed) == sizeof(MDBX_cursor *), "oops");
  const int err = MDBX_LIKELY(!cursors_array.empty())
                      ? ::mdbx_cursor_distribute(from, to, const_cast<MDBX_cursor **>(&cursors_array[0].handle_),
                                                 cursors_array.size(), deepness)
                      : MDBX_EINVAL;
  return error::boolean_or_throw(err);
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
