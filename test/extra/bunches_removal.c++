/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <vector>

#if defined(__cpp_lib_int_pow2) && __cpp_lib_int_pow2 >= 202002L
#include <bit>
#endif /* __cpp_lib_int_pow2 >= 202002L */

#if 1 || defined(ENABLE_MEMCHECK) || defined(MDBX_CI) || MDBX_DEBUG || !defined(NDEBUG) || defined(__APPLE__) ||       \
    defined(_WIN32)
#define DEEP 4
#else
#define DEEP 5
#endif

static ::std::ostream &operator<<(::std::ostream &out, const MDBX_bunch_action_t op) {
  static const char *const str[] = {
      "DELETE_CURRENT_VALUE",
      "DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING",
      "DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING",
      "DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING",
      "DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING",
      "DELETE_CURRENT_MULTIVAL_ALL",
      "DELETE_BEFORE_EXCLUDING",
      "MDBX_DELETE_BEFORE_INCLUDING",
      "MDBX_DELETE_AFTER_INCLUDING",
      "MDBX_DELETE_AFTER_EXCLUDING",
      "MDBX_DELETE_WHOLE",
  };
  return out << str[op];
}

static void logger_nofmt(MDBX_log_level_t loglevel, const char *function, int line, const char *msg,
                         unsigned length) noexcept {
  (void)length;
  (void)loglevel;
  fflush(nullptr);
  std::cout << function << ":" << line << " " << msg;
  std::cout.flush();
}

static char log_buffer[1024];

MDBX_MAYBE_UNUSED static std::string format_va(const char *fmt, va_list ap) {
  va_list ones;
  va_copy(ones, ap);
#ifdef _MSC_VER
  int needed = _vscprintf(fmt, ap);
#else
  int needed = vsnprintf(nullptr, 0, fmt, ap);
#endif
  assert(needed >= 0);
  std::string result;
  result.reserve(size_t(needed + 1));
  result.resize(size_t(needed), '\0');
  assert(int(result.capacity()) > needed);
  int actual = vsnprintf(const_cast<char *>(result.data()), result.capacity(), fmt, ones);
  assert(actual == needed);
  (void)actual;
  va_end(ones);
  return result;
}

MDBX_MAYBE_UNUSED static void debug(int line, const char *msg, ...) {
#if MDBX_DEBUG || !defined(NDEBUG)
  va_list ap;
  va_start(ap, msg);
  std::string result = format_va(msg, ap);
  va_end(ap);
  std::cout << "line " << line << ": " << result << std::endl;
#else
  (void)line;
  (void)msg;
#endif
}

MDBX_NORETURN static void unexpected(unsigned line) {
  std::cout.flush();
  std::cerr.flush();
  throw std::runtime_error(std::string("unexpected at line ") + std::to_string(line));
}

static bool failed(unsigned line) {
  std::cout.flush();
  std::cerr << "failed ad line " << line << std::endl;
  std::cerr.flush();
  return false;
}

//---------------------------------------------------------------------------------------------------------------------

using buffer = mdbx::default_buffer;
using buffer_pair = mdbx::buffer_pair<buffer>;

std::default_random_engine prng(42);

struct case_kind {
  const mdbx::map_handle table;
  const mdbx::slice name;
  const MDBX_db_flags_t flags;
  case_kind(mdbx::txn txn, const char *name, const mdbx::key_mode key_mode, const mdbx::value_mode value_mode) noexcept
      : table(txn.create_map(name, key_mode, value_mode)), name(name), flags(txn.get_map_flags(table).flags) {}
  case_kind(const case_kind &) = default;
  case_kind(case_kind &&) = default;
  virtual buffer_pair kv(size_t base) = 0;
  virtual ~case_kind() {}
  bool is_multivalue() const { return (flags & MDBX_DUPSORT) != 0; }
  bool is_ordinal() const { return (flags & (MDBX_INTEGERKEY | MDBX_INTEGERDUP)) != 0; }
  static uint64_t mix(size_t base) {
    auto x = base * UINT64_C(6364136223846793005);
    /* Pelle Evensen's mixer, https://bit.ly/2HOfynt */
    x ^= (x << 39 | x >> 25) ^ (x << 14 | x >> 50);
    x *= UINT64_C(0xA24BAED4963EE407);
    x ^= (x << 40 | x >> 24) ^ (x << 15 | x >> 49);
    x *= UINT64_C(0x9FB21C651E98DF25);
    return (x + 1442695040888963407) ^ x >> 28;
  }

  static uint64_t chop(uint64_t x, size_t w) {
    auto h = uint64_t(1) << w;
    return h + (x & (h - 1));
  }
};

struct case_usual : public case_kind {
  case_usual(mdbx::txn txn) noexcept : case_kind(txn, "usual", mdbx::key_mode::usual, mdbx::value_mode::single) {}
  buffer_pair kv(size_t base) override { return buffer_pair(buffer::base58(base), buffer::base58(~base)); }
};

struct case_multi : public case_kind {
  case_multi(mdbx::txn txn) noexcept : case_kind(txn, "multi", mdbx::key_mode::usual, mdbx::value_mode::multi) {}
  buffer_pair kv(size_t base) override {
    auto x = mix(base);
    auto w = std::bit_width(base | 42);
    auto k = chop(x, w / 2);
    auto v = chop(x >> w / 2, w - w / 2);
    return buffer_pair(buffer::base58(k), buffer::base58(v));
  }
};

struct case_ordinal : public case_kind {
  case_ordinal(mdbx::txn txn) noexcept
      : case_kind(txn, "ordinal", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal) {}
  buffer_pair kv(size_t base) override {
    auto x = mix(base);
    auto w = std::bit_width(base | 42);
    auto k = chop(x, w / 2);
    auto v = chop(x >> w / 2, w - w / 2);
    return buffer_pair(buffer::wrap(uint64_t(k)), buffer::wrap(uint64_t(v)));
  }
};

struct less {
  const MDBX_db_flags_t table_flags;
  less(MDBX_db_flags_t table_flags) : table_flags(table_flags) {}
  less(const less &) = default;
  bool operator()(const buffer_pair &a, const buffer_pair &b) const {
    intptr_t cmp;
    if (table_flags & MDBX_INTEGERKEY)
      cmp = (a.key.as_int64_adapt() == b.key.as_int64_adapt())  ? 0
            : (a.key.as_int64_adapt() < b.key.as_int64_adapt()) ? -1
                                                                : 1;
    else
      cmp = mdbx::slice::compare_lexicographically(a.key, b.key);

    if (cmp == 0) {
      if (table_flags & MDBX_INTEGERDUP)
        cmp = (a.value.as_int64_adapt() == b.value.as_int64_adapt())  ? 0
              : (a.value.as_int64_adapt() < b.value.as_int64_adapt()) ? -1
                                                                      : 1;
      else
        cmp = mdbx::slice::compare_lexicographically(a.value, b.value);
    }

    return cmp < 0;
  }
};

using verifier = std::set<buffer_pair, less>;

static bool turn(mdbx::txn txn, const case_kind &kvg, verifier &checker, const MDBX_bunch_action_t op) {
  const size_t size_before = checker.size();
  if (size_before != txn.get_map_stat(kvg.table).ms_entries)
    unexpected(__LINE__);

  const size_t pos = size_before ? prng() % size_before : 0;
  auto cursor = txn.open_cursor(kvg.table);
  auto iter = checker.begin();
  cursor.to_first(false);
  for (auto i = pos; i > 0; --i) {
    cursor.to_next();
    ++iter;
  }
  debug(__LINE__, "turn: size %zu, pos %zu", size_before, pos);

  const buffer_pair current = cursor.current();
  if (current != *iter)
    unexpected(__LINE__);

  auto prev_cursor = cursor.clone();
  const buffer_pair prev = prev_cursor.to_previous(false);
  auto prev_iter = iter;
  if (iter != checker.begin())
    --prev_iter;
  else
    prev_iter = checker.end();

  auto prev_prev_cursor = prev_cursor.clone();
  const buffer_pair prev_prev = prev_prev_cursor.to_previous(false);
  auto prev_prev_iter = prev_iter;
  if (prev_iter != checker.end() && prev_iter != checker.begin())
    --prev_prev_iter;
  else
    prev_prev_iter = checker.end();

  auto next_cursor = cursor.clone();
  const buffer_pair next = next_cursor.to_next(false);
  auto next_iter = iter;
  if (iter != checker.end())
    ++next_iter;

  auto next_next_cursor = next_cursor.clone();
  const buffer_pair next_next = next_next_cursor.to_next(false);
  auto next_next_iter = next_iter;
  if (next_iter != checker.end())
    ++next_next_iter;

  size_t count = 0xDEADBEEF;
  int err = mdbx_cursor_bunch_delete(cursor, op, &count);
  if (err != MDBX_SUCCESS)
    return failed(__LINE__);

  switch (op) {
  default:
    unexpected(__LINE__);
  case MDBX_DELETE_CURRENT_VALUE:
    checker.erase(iter);
    break;
  case MDBX_DELETE_WHOLE:
    checker.clear();
    break;

  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING:
    if (iter == checker.begin())
      break;
    --iter;
    [[fallthrough]];
  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING:
    while (iter != checker.end() && iter->key == current.key) {
      auto target = iter;
      if (iter != checker.begin())
        --iter;
      else
        iter = checker.end();
      checker.erase(target);
    }
    break;

  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING:
    if (iter == checker.end() || ++iter == checker.end())
      break;
    [[fallthrough]];
  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING:
    while (iter != checker.end() && iter->key == current.key) {
      auto target = iter;
      ++iter;
      checker.erase(target);
    }
    break;

  case MDBX_DELETE_CURRENT_MULTIVAL_ALL:
    while (iter != checker.begin()) {
      auto target = iter;
      --target;
      if (target->key != current.key)
        break;
      iter = target;
    }
    while (iter != checker.end() && iter->key == current.key) {
      auto target = iter;
      ++iter;
      checker.erase(target);
    }
    break;

  case MDBX_DELETE_BEFORE_EXCLUDING:
    if (iter == checker.begin())
      break;
    --iter;
    [[fallthrough]];
  case MDBX_DELETE_BEFORE_INCLUDING:
    while (iter != checker.end()) {
      auto target = iter;
      if (iter != checker.begin())
        --iter;
      else
        iter = checker.end();
      checker.erase(target);
    }
    break;

  case MDBX_DELETE_AFTER_EXCLUDING:
    if (iter == checker.end() || ++iter == checker.end())
      break;
    [[fallthrough]];
  case MDBX_DELETE_AFTER_INCLUDING:
    while (iter != checker.end()) {
      auto target = iter;
      ++iter;
      checker.erase(target);
    }
    break;
  }

  if (checker.size() != txn.get_map_stat(kvg.table).ms_entries)
    return failed(__LINE__);

  if (checker.size() + count != size_before)
    return failed(__LINE__);

  cursor.to_first(false);
  for (iter = checker.begin(); iter != checker.end(); ++iter) {
    if (iter == prev_prev_iter && *iter != prev_prev_cursor.current())
      return failed(__LINE__);
    if (iter == prev_iter && *iter != prev_cursor.current())
      return failed(__LINE__);
    if (*iter != cursor.current())
      return failed(__LINE__);
    if (iter == next_iter && *iter != next_cursor.current())
      return failed(__LINE__);
    if (iter == next_next_iter && *iter != next_next_cursor.current())
      return failed(__LINE__);
    cursor.to_next(false);
  }

  cursor.to_last(false);
  for (iter = checker.end(); iter != checker.begin();) {
    --iter;
    if (iter == prev_prev_iter && *iter != prev_prev_cursor.current())
      return failed(__LINE__);
    if (iter == prev_iter && *iter != prev_cursor.current())
      return failed(__LINE__);
    if (*iter != cursor.current())
      return failed(__LINE__);
    if (iter == next_iter && *iter != next_cursor.current())
      return failed(__LINE__);
    if (iter == next_next_iter && *iter != next_next_cursor.current())
      return failed(__LINE__);
    cursor.to_previous(false);
  }

  return true;
}

static bool probe(mdbx::env env, case_kind &kvg, unsigned deep, const MDBX_bunch_action_t op) {
  auto txn = env.start_write();
  txn.clear_map(kvg.table);
  verifier checker(less(kvg.flags));
  for (size_t serial = 0; txn.get_map_stat(kvg.table).ms_depth < deep || (deep && checker.size() < 42); ++serial) {
    auto pair = kvg.kv(serial);
    txn.upsert(kvg.table, pair.key, pair.value);
    checker.insert(std::move(pair));
  }
  txn.commit();

  const bool is_simple =
      op == MDBX_DELETE_CURRENT_VALUE || (!kvg.is_multivalue() && op <= MDBX_DELETE_CURRENT_MULTIVAL_ALL);

  // выполняем удаления в одной транзакции
  if (!checker.empty()) {
    txn = env.start_write();
    auto copy = checker;
    size_t n = 0, unchanged = 0;
    do {
      const auto prev_size = copy.size();
      debug(__LINE__, ">> #%zu, size %zu, deep %zu", n, prev_size, txn.get_map_stat(kvg.table).ms_depth);
      if (!turn(txn, kvg, copy, op))
        return false;
      debug(__LINE__, "<< #%zu, size %zu, deep %zu", n, copy.size(), txn.get_map_stat(kvg.table).ms_depth);
      unchanged = (prev_size != copy.size()) ? 0 : unchanged + 1;
    } while (!copy.empty() && (++n < 10 || !is_simple) && unchanged < copy.size() + 2);
    txn.abort();
  }

  // выполняем удаления в отдельных транзакциях
  size_t n = 0, unchanged = 0;
  do {
    const auto prev_size = checker.size();
    txn = env.start_write();
    debug(__LINE__, ">> #%zu, size %zu, deep %zu", n, prev_size, txn.get_map_stat(kvg.table).ms_depth);
    if (!turn(txn, kvg, checker, op))
      return false;
    debug(__LINE__, "<< #%zu, size %zu, deep %zu", n, checker.size(), txn.get_map_stat(kvg.table).ms_depth);
    txn.commit();
    unchanged = (prev_size != checker.size()) ? 0 : unchanged + 1;
  } while (!checker.empty() && (++n < 10 || !is_simple) && unchanged < checker.size() + 2);

  return true;
}

using case_set = std::vector<std::unique_ptr<case_kind>>;

static bool test(mdbx::env env, case_set &set, unsigned deep) {
  bool ok = true;
  for (size_t op = MDBX_DELETE_CURRENT_VALUE + (deep > 1); op < MDBX_DELETE_WHOLE; ++op) {
    for (auto &kvg : set) {
      std::cout << kvg->name << ": " << MDBX_bunch_action_t(op) << " deep " << deep << std::endl;
      ok = probe(env, *kvg, deep, MDBX_bunch_action_t(op)) && ok;
    }
  }

  return ok;
}

int doit() {
#if 1
  std::random_device random;
  std::seed_seq seed({random(), random(), random(), random(), random()});
#else
  std::seed_seq seed({42});
#endif

  std::cout << "seed ";
  seed.param(std::ostream_iterator<size_t>(std::cout, ", "));
  std::cout << std::endl;
  prng.seed(seed);

  mdbx::path db_filename = "test-range-delete";
  mdbx::env_managed::remove(db_filename);

  mdbx::env_managed::create_parameters create_parameters;
  create_parameters.geometry.pagesize = mdbx::env::geometry::minimal_value;
  mdbx::env_managed env(db_filename, create_parameters,
                        mdbx::env::operate_parameters(3, 0, mdbx::env::nested_transactions,
                                                      mdbx::env::durability::whole_fragile,
                                                      mdbx::env::reclaiming_options()));
  if (env.get_info().mi_dxb_pagesize != 256)
    unexpected(__LINE__);

  auto txn = env.start_write();
  case_set set;
  set.emplace_back(new case_usual(txn));
  set.emplace_back(new case_multi(txn));
  set.emplace_back(new case_ordinal(txn));
  txn.commit();

  bool ok = true;
  for (auto deep = 1; deep <= DEEP && ok; ++deep)
    ok = test(env, set, deep);

  if (!ok) {
    std::cerr << "Fail\n";
    return EXIT_FAILURE;
  }
  std::cout << "OK\n";
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  mdbx_setup_debug_nofmt(MDBX_LOG_NOTICE /* MDBX_LOG_VERBOSE */, MDBX_DBG_ASSERT, logger_nofmt, log_buffer,
                         sizeof(log_buffer));
  try {
    return doit();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
