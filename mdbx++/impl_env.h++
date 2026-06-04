// > dist-cutoff-begin
#pragma once
#include "decl_cursor.h++"
#include "decl_env.h++"
#include "decl_txn.h++"
namespace mdbx {
// < dist-cutoff-end

MDBX_CXX11_CONSTEXPR env::env(MDBX_env *ptr) noexcept : handle_(ptr) {}

inline env &env::operator=(env &&other) noexcept {
  if (this != &other) {
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

inline env::env(env &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

inline env::~env() noexcept {
#if (defined(MDBX_CHECKING) && MDBX_CHECKING > 0) || (defined(MDBX_DEBUG) && MDBX_DEBUG > 0)
  handle_ = reinterpret_cast<MDBX_env *>(uintptr_t(0xDeadBeef));
#endif
}

inline env::geometry &env::geometry::make_fixed(intptr_t size) noexcept {
  size_lower = size_now = size_upper = size;
  growth_step = shrink_threshold = 0;
  return *this;
}

inline env::geometry &env::geometry::make_dynamic(intptr_t lower, intptr_t upper) noexcept {
  size_now = size_lower = lower;
  size_upper = upper;
  growth_step = shrink_threshold = default_value;
  return *this;
}

inline env::reclaiming_options env::operate_parameters::reclaiming_from_flags(MDBX_env_flags_t flags) noexcept {
  return reclaiming_options(flags);
}

inline env::operate_options env::operate_parameters::options_from_flags(MDBX_env_flags_t flags) noexcept {
  return operate_options(flags);
}

inline size_t env::limits::pagesize_min() noexcept { return MDBX_MIN_PAGESIZE; }

inline size_t env::limits::pagesize_max() noexcept { return MDBX_MAX_PAGESIZE; }

inline size_t env::limits::dbsize_min(intptr_t pagesize) {
  const intptr_t result = mdbx_limits_dbsize_min(pagesize);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::dbsize_max(intptr_t pagesize) {
  const intptr_t result = mdbx_limits_dbsize_max(pagesize);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::key_min(MDBX_db_flags_t flags) noexcept { return (flags & MDBX_INTEGERKEY) ? 4 : 0; }

inline size_t env::limits::key_min(key_mode mode) noexcept { return key_min(MDBX_db_flags_t(mode)); }

inline size_t env::limits::key_max(intptr_t pagesize, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_limits_keysize_max(pagesize, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::key_max(intptr_t pagesize, key_mode mode) {
  return key_max(pagesize, MDBX_db_flags_t(mode));
}

inline size_t env::limits::key_max(const env &env, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_env_get_maxkeysize_ex(env, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::key_max(const env &env, key_mode mode) { return key_max(env, MDBX_db_flags_t(mode)); }

inline size_t env::limits::value_min(MDBX_db_flags_t flags) noexcept { return (flags & MDBX_INTEGERDUP) ? 4 : 0; }

inline size_t env::limits::value_min(value_mode mode) noexcept { return value_min(MDBX_db_flags_t(mode)); }

inline size_t env::limits::value_max(intptr_t pagesize, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_limits_valsize_max(pagesize, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::value_max(intptr_t pagesize, value_mode mode) {
  return value_max(pagesize, MDBX_db_flags_t(mode));
}

inline size_t env::limits::value_max(const env &env, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_env_get_maxvalsize_ex(env, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::value_max(const env &env, value_mode mode) { return value_max(env, MDBX_db_flags_t(mode)); }

inline size_t env::limits::pairsize4page_max(intptr_t pagesize, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_limits_pairsize4page_max(pagesize, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::pairsize4page_max(intptr_t pagesize, value_mode mode) {
  return pairsize4page_max(pagesize, MDBX_db_flags_t(mode));
}

inline size_t env::limits::pairsize4page_max(const env &env, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_env_get_pairsize4page_max(env, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::pairsize4page_max(const env &env, value_mode mode) {
  return pairsize4page_max(env, MDBX_db_flags_t(mode));
}

inline size_t env::limits::valsize4page_max(intptr_t pagesize, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_limits_valsize4page_max(pagesize, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::valsize4page_max(intptr_t pagesize, value_mode mode) {
  return valsize4page_max(pagesize, MDBX_db_flags_t(mode));
}

inline size_t env::limits::valsize4page_max(const env &env, MDBX_db_flags_t flags) {
  const intptr_t result = mdbx_env_get_valsize4page_max(env, flags);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::valsize4page_max(const env &env, value_mode mode) {
  return valsize4page_max(env, MDBX_db_flags_t(mode));
}

inline size_t env::limits::transaction_size_max(intptr_t pagesize) {
  const intptr_t result = mdbx_limits_txnsize_max(pagesize);
  if (result < 0)
    MDBX_CXX20_UNLIKELY error::throw_exception(MDBX_EINVAL);
  return static_cast<size_t>(result);
}

inline size_t env::limits::max_map_handles(void) { return MDBX_MAX_DBI; }

inline env::operate_parameters env::get_operation_parameters() const {
  const auto flags = get_flags();
  return operate_parameters(max_maps(), max_readers(), operate_parameters::mode_from_flags(flags),
                            operate_parameters::durability_from_flags(flags),
                            operate_parameters::reclaiming_from_flags(flags),
                            operate_parameters::options_from_flags(flags));
}

inline env::mode env::get_mode() const { return operate_parameters::mode_from_flags(get_flags()); }

inline env::durability env::get_durability() const {
  return env::operate_parameters::durability_from_flags(get_flags());
}

inline env::reclaiming_options env::get_reclaiming() const {
  return env::operate_parameters::reclaiming_from_flags(get_flags());
}

inline env::operate_options env::get_options() const {
  return env::operate_parameters::options_from_flags(get_flags());
}

inline env::stat env::get_stat() const {
  env::stat r;
  error::success_or_throw(::mdbx_env_stat_ex(handle_, nullptr, &r, sizeof(r)));
  return r;
}

inline env::stat env::get_stat(const txn &txn) const {
  env::stat r;
  error::success_or_throw(::mdbx_env_stat_ex(handle_, txn, &r, sizeof(r)));
  return r;
}

inline env::info env::get_info() const {
  env::info r;
  error::success_or_throw(::mdbx_env_info_ex(handle_, nullptr, &r, sizeof(r)));
  return r;
}

inline env::info env::get_info(const txn &txn) const {
  env::info r;
  error::success_or_throw(::mdbx_env_info_ex(handle_, txn, &r, sizeof(r)));
  return r;
}

inline filehandle env::get_filehandle() const {
  filehandle fd;
  error::success_or_throw(::mdbx_env_get_fd(handle_, &fd));
  return fd;
}

inline MDBX_env_flags_t env::get_flags() const {
  unsigned bits = 0;
  error::success_or_throw(::mdbx_env_get_flags(handle_, &bits));
  return MDBX_env_flags_t(bits);
}

inline unsigned env::max_readers() const {
  unsigned r;
  error::success_or_throw(::mdbx_env_get_maxreaders(handle_, &r));
  return r;
}

inline unsigned env::max_maps() const {
  unsigned r;
  error::success_or_throw(::mdbx_env_get_maxdbs(handle_, &r));
  return r;
}

inline void *env::get_context() const noexcept { return mdbx_env_get_userctx(handle_); }

inline env &env::set_context(void *ptr) {
  error::success_or_throw(::mdbx_env_set_userctx(handle_, ptr));
  return *this;
}

inline env &env::set_sync_threshold(size_t bytes) {
  error::success_or_throw(::mdbx_env_set_syncbytes(handle_, bytes));
  return *this;
}

inline size_t env::sync_threshold() const {
  size_t bytes;
  error::success_or_throw(::mdbx_env_get_syncbytes(handle_, &bytes));
  return bytes;
}

inline env &env::set_sync_period__seconds_16dot16(unsigned seconds_16dot16) {
  error::success_or_throw(::mdbx_env_set_syncperiod(handle_, seconds_16dot16));
  return *this;
}

inline unsigned env::sync_period__seconds_16dot16() const {
  unsigned seconds_16dot16;
  error::success_or_throw(::mdbx_env_get_syncperiod(handle_, &seconds_16dot16));
  return seconds_16dot16;
}

inline env &env::set_sync_period__seconds_double(double seconds) {
  return set_sync_period__seconds_16dot16(unsigned(seconds * 65536));
}

inline double env::sync_period__seconds_double() const { return sync_period__seconds_16dot16() / 65536.0; }

#if __cplusplus >= 201103L
inline env &env::set_sync_period(const duration &period) { return set_sync_period__seconds_16dot16(period.count()); }

inline duration env::sync_period() const { return duration(sync_period__seconds_16dot16()); }
#endif

inline env &env::set_extra_option(enum env::extra_runtime_option option, uint64_t value) {
  error::success_or_throw(::mdbx_env_set_option(handle_, ::MDBX_option_t(option), value));
  return *this;
}

inline uint64_t env::extra_option(enum env::extra_runtime_option option) const {
  uint64_t value;
  error::success_or_throw(::mdbx_env_get_option(handle_, ::MDBX_option_t(option), &value));
  return value;
}

inline env &env::alter_flags(MDBX_env_flags_t flags, bool on_off) {
  error::success_or_throw(::mdbx_env_set_flags(handle_, flags, on_off));
  return *this;
}

inline env &env::set_geometry(const geometry &geo) {
  error::success_or_throw(::mdbx_env_set_geometry(handle_, geo.size_lower, geo.size_now, geo.size_upper,
                                                  geo.growth_step, geo.shrink_threshold, geo.pagesize));
  return *this;
}

inline bool env::sync_to_disk(bool force, bool nonblock) {
  const int err = ::mdbx_env_sync_ex(handle_, force, nonblock);
  switch (err) {
  case MDBX_SUCCESS /* flush done */:
  case MDBX_RESULT_TRUE /* no data pending for flush to disk */:
    return true;
  case MDBX_BUSY /* the environment is used by other thread */:
    return false;
  default:
    MDBX_CXX20_UNLIKELY error::throw_exception(err);
  }
}

inline void env::close_map(const map_handle &handle) { error::success_or_throw(::mdbx_dbi_close(*this, handle.dbi)); }

MDBX_CXX11_CONSTEXPR
env::reader_info::reader_info(int slot, mdbx_pid_t pid, mdbx_tid_t thread, uint64_t txnid, uint64_t lag, size_t used,
                              size_t retained) noexcept
    : slot(slot), pid(pid), thread(thread), transaction_id(txnid), transaction_lag(lag), bytes_used(used),
      bytes_retained(retained) {}

template <typename VISITOR> inline int env::enumerate_readers(VISITOR &visitor) {
  struct reader_visitor_thunk : public exception_thunk {
    VISITOR &visitor_;
    static int cb(void *ctx, int number, int slot, mdbx_pid_t pid, mdbx_tid_t thread, uint64_t txnid, uint64_t lag,
                  size_t used, size_t retained) noexcept {
      reader_visitor_thunk *thunk = static_cast<reader_visitor_thunk *>(ctx);
      assert(thunk->is_clean());
      try {
        const reader_info info(slot, pid, thread, txnid, lag, used, retained);
        return loop_control(thunk->visitor_(info, number));
      } catch (... /* capture any exception to rethrow it over C code */) {
        thunk->capture();
        return loop_control::exit_loop;
      }
    }
    MDBX_CXX11_CONSTEXPR reader_visitor_thunk(VISITOR &visitor) noexcept : visitor_(visitor) {}
  };
  reader_visitor_thunk thunk(visitor);
  const auto rc = ::mdbx_reader_list(*this, thunk.cb, &thunk);
  thunk.rethrow_captured();
  return rc;
}

inline unsigned env::check_readers() {
  int dead_count;
  error::throw_on_failure(::mdbx_reader_check(*this, &dead_count));
  MDBX_INLINE_API_ASSERT(dead_count >= 0);
  return static_cast<unsigned>(dead_count);
}

inline env &env::set_HandleSlowReaders(MDBX_hsr_func cb) {
  error::success_or_throw(::mdbx_env_set_hsr(handle_, cb));
  return *this;
}

inline MDBX_hsr_func env::get_HandleSlowReaders() const noexcept { return ::mdbx_env_get_hsr(handle_); }

inline txn_managed env::start_read() const {
  ::MDBX_txn *ptr;
  error::success_or_throw(::mdbx_txn_begin(handle_, nullptr, MDBX_TXN_RDONLY, &ptr));
  MDBX_INLINE_API_ASSERT(ptr != nullptr);
  return txn_managed(ptr);
}

inline txn_managed env::prepare_read() const {
  ::MDBX_txn *ptr;
  error::success_or_throw(::mdbx_txn_begin(handle_, nullptr, MDBX_TXN_RDONLY_PREPARE, &ptr));
  MDBX_INLINE_API_ASSERT(ptr != nullptr);
  return txn_managed(ptr);
}

inline txn_managed env::start_write(bool dont_wait) {
  ::MDBX_txn *ptr;
  error::success_or_throw(
      ::mdbx_txn_begin(handle_, nullptr, dont_wait ? MDBX_TXN_READWRITE | MDBX_TXN_TRY : MDBX_TXN_READWRITE, &ptr));
  MDBX_INLINE_API_ASSERT(ptr != nullptr || dont_wait);
  return txn_managed(ptr);
}

inline txn_managed env::start_write(txn &parent) {
  ::MDBX_txn *ptr;
  error::success_or_throw(::mdbx_txn_begin(handle_, parent, MDBX_TXN_READWRITE, &ptr));
  MDBX_INLINE_API_ASSERT(ptr != nullptr);
  return txn_managed(ptr);
}

inline txn_managed env::try_start_write() { return start_write(true); }

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
