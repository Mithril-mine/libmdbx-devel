// > dist-cutoff-begin
#pragma once
#include "decl_cursor.h++"
#include "decl_env.h++"
#include "decl_txn.h++"
namespace mdbx {
// < dist-cutoff-end

MDBX_CXX11_CONSTEXPR txn::txn(MDBX_txn *ptr) noexcept : handle_(ptr) {}

inline txn &txn::operator=(txn &&other) noexcept {
  if (this != &other) {
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

inline txn::txn(txn &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

inline txn::~txn() noexcept {
#if (defined(MDBX_CHECKING) && MDBX_CHECKING > 0) || (defined(MDBX_DEBUG) && MDBX_DEBUG > 0)
  handle_ = reinterpret_cast<MDBX_txn *>(uintptr_t(0xDeadBeef));
#endif
}

inline void *txn::get_context() const noexcept { return mdbx_txn_get_userctx(handle_); }

inline txn &txn::set_context(void *ptr) {
  error::success_or_throw(::mdbx_txn_set_userctx(handle_, ptr));
  return *this;
}

inline bool txn::is_dirty(const void *ptr) const {
  int err = ::mdbx_is_dirty(handle_, ptr);
  switch (err) {
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  case MDBX_RESULT_TRUE:
    return true;
  case MDBX_RESULT_FALSE:
    return false;
  }
}

inline env txn::env() const noexcept { return ::mdbx_txn_env(handle_); }

inline MDBX_txn_flags_t txn::flags() const {
  const int bits = mdbx_txn_flags(handle_);
  error::throw_on_failure((bits != -1) ? MDBX_SUCCESS : MDBX_BAD_TXN);
  return static_cast<MDBX_txn_flags_t>(bits);
}

inline uint64_t txn::id() const {
  const uint64_t txnid = mdbx_txn_id(handle_);
  error::throw_on_failure(txnid ? MDBX_SUCCESS : MDBX_BAD_TXN);
  return txnid;
}

inline void txn::reset_reading() { error::success_or_throw(::mdbx_txn_reset(handle_)); }

inline void txn::make_broken() { error::success_or_throw(::mdbx_txn_break(handle_)); }

inline void txn::renew_reading() { error::success_or_throw(::mdbx_txn_renew(handle_)); }

inline txn_managed txn::clone(void *context) const {
  MDBX_txn *ptr = nullptr;
  error::success_or_throw(::mdbx_txn_clone(handle_, &ptr, context));
  MDBX_INLINE_API_ASSERT(ptr != nullptr);
  return txn_managed(ptr);
}

inline void txn::clone(txn_managed &txn_for_renew_into_clone, void *context) const {
  error::throw_on_nullptr(txn_for_renew_into_clone.handle_, MDBX_BAD_TXN);
  error::success_or_throw(::mdbx_txn_clone(handle_, &txn_for_renew_into_clone.handle_, context));
  MDBX_INLINE_API_ASSERT(txn_for_renew_into_clone.handle_ != nullptr);
}

inline void txn::park_reading(bool autounpark) { error::success_or_throw(::mdbx_txn_park(handle_, autounpark)); }

inline bool txn::unpark_reading(bool restart_if_ousted) {
  return error::boolean_or_throw(::mdbx_txn_unpark(handle_, restart_if_ousted));
}

inline txn::info txn::get_info(bool scan_reader_lock_table) const {
  txn::info r;
  error::success_or_throw(::mdbx_txn_info(handle_, &r, scan_reader_lock_table));
  return r;
}

inline cursor_managed txn::open_cursor(map_handle map) const {
  MDBX_cursor *ptr;
  error::success_or_throw(::mdbx_cursor_open(handle_, map.dbi, &ptr));
  return cursor_managed(ptr);
}

inline size_t txn::release_all_cursors(bool unbind) const {
  size_t count;
  error::success_or_throw(::mdbx_txn_release_all_cursors_ex(handle_, unbind, &count));
  return count;
}

inline map_handle txn::open_map(const slice &name, const ::mdbx::key_mode key_mode,
                                const ::mdbx::value_mode value_mode) const {
  map_handle map;
  error::success_or_throw(
      ::mdbx_dbi_open2(handle_, name, MDBX_db_flags_t(key_mode) | MDBX_db_flags_t(value_mode), &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline map_handle txn::open_map(const char *name, const ::mdbx::key_mode key_mode,
                                const ::mdbx::value_mode value_mode) const {
  map_handle map;
  error::success_or_throw(
      ::mdbx_dbi_open(handle_, name, MDBX_db_flags_t(key_mode) | MDBX_db_flags_t(value_mode), &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline map_handle txn::open_map_accede(const slice &name) const {
  map_handle map;
  error::success_or_throw(::mdbx_dbi_open2(handle_, name, MDBX_DB_ACCEDE, &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline map_handle txn::open_map_accede(const char *name) const {
  map_handle map;
  error::success_or_throw(::mdbx_dbi_open(handle_, name, MDBX_DB_ACCEDE, &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline map_handle txn::create_map(const slice &name, const ::mdbx::key_mode key_mode,
                                  const ::mdbx::value_mode value_mode) {
  map_handle map;
  error::success_or_throw(
      ::mdbx_dbi_open2(handle_, name, MDBX_CREATE | MDBX_db_flags_t(key_mode) | MDBX_db_flags_t(value_mode), &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline map_handle txn::create_map(const char *name, const ::mdbx::key_mode key_mode,
                                  const ::mdbx::value_mode value_mode) {
  map_handle map;
  error::success_or_throw(
      ::mdbx_dbi_open(handle_, name, MDBX_CREATE | MDBX_db_flags_t(key_mode) | MDBX_db_flags_t(value_mode), &map.dbi));
  MDBX_INLINE_API_ASSERT(map.dbi != 0);
  return map;
}

inline void txn::drop_map(map_handle map) { error::success_or_throw(::mdbx_drop(handle_, map.dbi, true)); }

inline void txn::clear_map(map_handle map) { error::success_or_throw(::mdbx_drop(handle_, map.dbi, false)); }

inline void txn::rename_map(map_handle map, const char *new_name) {
  error::success_or_throw(::mdbx_dbi_rename(handle_, map, new_name));
}

inline void txn::rename_map(map_handle map, const slice &new_name) {
  error::success_or_throw(::mdbx_dbi_rename2(handle_, map, new_name));
}

inline map_handle txn::open_map(const ::std::string &name, const ::mdbx::key_mode key_mode,
                                const ::mdbx::value_mode value_mode) const {
  return open_map(slice(name), key_mode, value_mode);
}

inline map_handle txn::open_map_accede(const ::std::string &name) const { return open_map_accede(slice(name)); }

inline map_handle txn::create_map(const ::std::string &name, const ::mdbx::key_mode key_mode,
                                  const ::mdbx::value_mode value_mode) {
  return create_map(slice(name), key_mode, value_mode);
}

inline bool txn::drop_map(const ::std::string &name, bool throw_if_absent) {
  return drop_map(slice(name), throw_if_absent);
}

inline bool txn::clear_map(const ::std::string &name, bool throw_if_absent) {
  return clear_map(slice(name), throw_if_absent);
}

inline void txn::rename_map(map_handle map, const ::std::string &new_name) { return rename_map(map, slice(new_name)); }

inline txn::map_stat txn::get_map_stat(map_handle map) const {
  txn::map_stat r;
  error::success_or_throw(::mdbx_dbi_stat(handle_, map.dbi, &r, sizeof(r)));
  return r;
}

inline uint32_t txn::get_tree_deepmask(map_handle map) const {
  uint32_t r;
  error::success_or_throw(::mdbx_dbi_dupsort_depthmask(handle_, map.dbi, &r));
  return r;
}

inline map_handle::info txn::get_map_flags(map_handle map) const {
  unsigned flags, state;
  error::success_or_throw(::mdbx_dbi_flags_ex(handle_, map.dbi, &flags, &state));
  return map_handle::info(MDBX_db_flags_t(flags), MDBX_dbi_state_t(state));
}

inline txn &txn::put_canary(const txn::canary &canary) {
  error::success_or_throw(::mdbx_canary_put(handle_, &canary));
  return *this;
}

inline txn::canary txn::get_canary() const {
  txn::canary r;
  error::success_or_throw(::mdbx_canary_get(handle_, &r));
  return r;
}

inline uint64_t txn::sequence(map_handle map) const {
  uint64_t result;
  error::success_or_throw(::mdbx_dbi_sequence(handle_, map.dbi, &result, 0));
  return result;
}

inline uint64_t txn::sequence(map_handle map, uint64_t increment) {
  uint64_t result;
  error::success_or_throw(::mdbx_dbi_sequence(handle_, map.dbi, &result, increment));
  return result;
}

inline int txn::compare_keys(map_handle map, const slice &a, const slice &b) const noexcept {
  return ::mdbx_cmp(handle_, map.dbi, &a, &b);
}

inline int txn::compare_values(map_handle map, const slice &a, const slice &b) const noexcept {
  return ::mdbx_dcmp(handle_, map.dbi, &a, &b);
}

inline int txn::compare_keys(map_handle map, const pair &a, const pair &b) const noexcept {
  return compare_keys(map, a.key, b.key);
}

inline int txn::compare_values(map_handle map, const pair &a, const pair &b) const noexcept {
  return compare_values(map, a.value, b.value);
}

inline slice txn::get(map_handle map, const slice &key) const {
  slice result;
  error::success_or_throw(::mdbx_get(handle_, map.dbi, &key, &result));
  return result;
}

inline slice txn::get(map_handle map, slice key, size_t &values_count) const {
  slice result;
  error::success_or_throw(::mdbx_get_ex(handle_, map.dbi, &key, &result, &values_count));
  return result;
}

inline slice txn::get(map_handle map, const slice &key, const slice &value_at_absence) const {
  slice result;
  const int err = ::mdbx_get(handle_, map.dbi, &key, &result);
  switch (err) {
  case MDBX_SUCCESS:
    return result;
  case MDBX_NOTFOUND:
    return value_at_absence;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline slice txn::get(map_handle map, slice key, size_t &values_count, const slice &value_at_absence) const {
  slice result;
  const int err = ::mdbx_get_ex(handle_, map.dbi, &key, &result, &values_count);
  switch (err) {
  case MDBX_SUCCESS:
    return result;
  case MDBX_NOTFOUND:
    return value_at_absence;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline pair_result txn::get_equal_or_great(map_handle map, const slice &key) const {
  pair result(key, slice());
  bool exact = !error::boolean_or_throw(::mdbx_get_equal_or_great(handle_, map.dbi, &result.key, &result.value));
  return pair_result(result.key, result.value, exact);
}

inline pair_result txn::get_equal_or_great(map_handle map, const slice &key, const slice &value_at_absence) const {
  pair result{key, slice()};
  const int err = ::mdbx_get_equal_or_great(handle_, map.dbi, &result.key, &result.value);
  switch (err) {
  case MDBX_SUCCESS:
    return pair_result{result.key, result.value, true};
  case MDBX_RESULT_TRUE:
    return pair_result{result.key, result.value, false};
  case MDBX_NOTFOUND:
    return pair_result{key, value_at_absence, false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline MDBX_error_t txn::put(map_handle map, const slice &key, slice *value, MDBX_put_flags_t flags) noexcept {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  return MDBX_error_t(::mdbx_put(handle_, map.dbi, &key, value, flags));
}

inline void txn::put(map_handle map, const slice &key, slice value, put_mode mode) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, &value, MDBX_put_flags_t(mode)));
}

inline void txn::insert(map_handle map, const slice &key, slice value) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, &value /* takes the present value in case MDBX_KEYEXIST */,
                              MDBX_put_flags_t(put_mode::insert_unique)));
}

inline value_result txn::try_insert(map_handle map, const slice &key, slice value) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  const int err = put(map, key, &value /* takes the present value in case MDBX_KEYEXIST */,
                      MDBX_put_flags_t(put_mode::insert_unique));
  switch (err) {
  case MDBX_SUCCESS:
    return value_result{slice(), true};
  case MDBX_KEYEXIST:
    return value_result{value, false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline slice txn::insert_reserve(map_handle map, const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, &result /* takes the present value in case MDBX_KEYEXIST */,
                              MDBX_put_flags_t(put_mode::insert_unique) | MDBX_RESERVE));
  return result;
}

inline value_result txn::try_insert_reserve(map_handle map, const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  const int err = put(map, key, &result /* takes the present value in case MDBX_KEYEXIST */,
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

inline void txn::upsert(map_handle map, const slice &key, const slice &value) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::upsert)));
}

inline slice txn::upsert_reserve(map_handle map, const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, &result, MDBX_put_flags_t(put_mode::upsert) | MDBX_RESERVE));
  return result;
}

inline void txn::update(map_handle map, const slice &key, const slice &value) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::update)));
}

inline bool txn::try_update(map_handle map, const slice &key, const slice &value) {
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  const int err = put(map, key, const_cast<slice *>(&value), MDBX_put_flags_t(put_mode::update));
  switch (err) {
  case MDBX_SUCCESS:
    return true;
  case MDBX_NOTFOUND:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline slice txn::update_reserve(map_handle map, const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  error::success_or_throw(put(map, key, &result, MDBX_put_flags_t(put_mode::update) | MDBX_RESERVE));
  return result;
}

inline value_result txn::try_update_reserve(map_handle map, const slice &key, size_t value_length) {
  slice result(nullptr, value_length);
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  const int err = put(map, key, &result, MDBX_put_flags_t(put_mode::update) | MDBX_RESERVE);
  switch (err) {
  case MDBX_SUCCESS:
    return value_result{result, true};
  case MDBX_NOTFOUND:
    return value_result{slice(), false};
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline bool txn::erase(map_handle map, const slice &key) {
  const int err = ::mdbx_del(handle_, map.dbi, &key, nullptr);
  switch (err) {
  case MDBX_SUCCESS:
    return true;
  case MDBX_NOTFOUND:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline bool txn::erase(map_handle map, const slice &key, const slice &value) {
  const int err = ::mdbx_del(handle_, map.dbi, &key, &value);
  switch (err) {
  case MDBX_SUCCESS:
    return true;
  case MDBX_NOTFOUND:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline void txn::replace(map_handle map, const slice &key, slice old_value, const slice &new_value) {
  error::success_or_throw(::mdbx_replace_ex(handle_, map.dbi, &key, const_cast<slice *>(&new_value), &old_value,
                                            MDBX_CURRENT | MDBX_NOOVERWRITE, nullptr, nullptr));
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY>
txn::extract(map_handle map, const slice &key,
             const typename buffer<ALLOCATOR, CAPACITY_POLICY>::allocator_type &alloc) {
  typename buffer<ALLOCATOR, CAPACITY_POLICY>::data_preserver result(alloc);
  error::success_or_throw(
      ::mdbx_replace_ex(handle_, map.dbi, &key, nullptr, &result.slice_, MDBX_CURRENT, result, &result), result);
  return result;
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY>
txn::replace(map_handle map, const slice &key, const slice &new_value,
             const typename buffer<ALLOCATOR, CAPACITY_POLICY>::allocator_type &alloc) {
  typename buffer<ALLOCATOR, CAPACITY_POLICY>::data_preserver result(alloc);
  error::success_or_throw(::mdbx_replace_ex(handle_, map.dbi, &key, const_cast<slice *>(&new_value), &result.slice_,
                                            MDBX_CURRENT, result, &result),
                          result);
  return result;
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY>
txn::replace_reserve(map_handle map, const slice &key, slice &new_value,
                     const typename buffer<ALLOCATOR, CAPACITY_POLICY>::allocator_type &alloc) {
  typename buffer<ALLOCATOR, CAPACITY_POLICY>::data_preserver result(alloc);
  error::success_or_throw(::mdbx_replace_ex(handle_, map.dbi, &key, &new_value, &result.slice_,
                                            MDBX_CURRENT | MDBX_RESERVE, result, &result),
                          result);
  return result;
}

inline void txn::append(map_handle map, const slice &key, const slice &value, bool multivalue_order_preserved) {
  error::success_or_throw(::mdbx_put(handle_, map.dbi, const_cast<slice *>(&key), const_cast<slice *>(&value),
                                     multivalue_order_preserved ? (MDBX_APPEND | MDBX_APPENDDUP) : MDBX_APPEND));
}

inline size_t txn::put_multiple_samelength(map_handle map, const slice &key, const size_t value_length,
                                           const void *values_array, size_t values_count, put_mode mode,
                                           bool allow_partial) {
  MDBX_val args[2] = {{const_cast<void *>(values_array), value_length}, {nullptr, values_count}};
  const int err = ::mdbx_put(handle_, map.dbi, const_cast<slice *>(&key), args, MDBX_put_flags_t(mode) | MDBX_MULTIPLE);
  switch (err) {
  case MDBX_SUCCESS:
    MDBX_CXX20_LIKELY break;
  case MDBX_KEYEXIST:
    if (allow_partial)
      break;
    mdbx_txn_break(handle_);
    MDBX_CXX17_FALLTHROUGH /* fallthrough */;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
  return args[1].iov_len /* done item count */;
}

inline ptrdiff_t txn::estimate(map_handle map, const pair &from, const pair &to) const {
  ptrdiff_t result;
  error::success_or_throw(mdbx_estimate_range(handle_, map.dbi, &from.key, &from.value, &to.key, &to.value, &result));
  return result;
}

inline ptrdiff_t txn::estimate(map_handle map, const slice &from, const slice &to) const {
  ptrdiff_t result;
  error::success_or_throw(mdbx_estimate_range(handle_, map.dbi, &from, nullptr, &to, nullptr, &result));
  return result;
}

inline ptrdiff_t txn::estimate_from_first(map_handle map, const slice &to) const {
  ptrdiff_t result;
  error::success_or_throw(mdbx_estimate_range(handle_, map.dbi, nullptr, nullptr, &to, nullptr, &result));
  return result;
}

inline ptrdiff_t txn::estimate_to_last(map_handle map, const slice &from) const {
  ptrdiff_t result;
  error::success_or_throw(mdbx_estimate_range(handle_, map.dbi, &from, nullptr, nullptr, nullptr, &result));
  return result;
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
