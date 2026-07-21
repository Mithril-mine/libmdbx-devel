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
using std::bit_width;
#else
static size_t bit_width(size_t v) {
  size_t r;
  for (r = 0; v > 0; ++r)
    v >>= 1;
  return r;
}
#endif /* __cpp_lib_int_pow2 >= 202002L */

#if 1 || defined(ENABLE_MEMCHECK) || defined(MDBX_CI) || MDBX_DEBUG > 0 || !defined(NDEBUG) || defined(__APPLE__) ||   \
    defined(_WIN32)
#define DEEP 4
#else
/* Осторожно, очень долго */
#define DEEP 5
#endif

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
#if MDBX_DEBUG > 0 || !defined(NDEBUG)
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

MDBX_MAYBE_UNUSED static bool failed(unsigned line) {
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
  buffer_pair kv(size_t base) override {
    return buffer_pair(buffer::base58(base).append('.').append(std::to_string(base)),
                       buffer::base58(~base).append(".usual"));
  }
};

struct case_multi : public case_kind {
  case_multi(mdbx::txn txn) noexcept : case_kind(txn, "multi", mdbx::key_mode::usual, mdbx::value_mode::multi) {}
  buffer_pair kv(size_t base) override {
    auto x = mix(base);
    auto w = bit_width(base | 42);
    auto k = chop(x, w / 2);
    auto v = chop(x >> w / 2, w - w / 2);
    return buffer_pair(buffer::base58(k).append('.').append(std::to_string(k)),
                       buffer::base58(v).append('.').append(std::to_string(v)));
  }
};

struct case_ordinal : public case_kind {
  case_ordinal(mdbx::txn txn) noexcept
      : case_kind(txn, "ordinal", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal) {}
  buffer_pair kv(size_t base) override {
    auto x = mix(base);
    auto w = bit_width(base | 42);
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
using iterator = verifier::iterator;

MDBX_MAYBE_UNUSED static bool eq(const iterator &iter, mdbx::cursor cursor, const char *caption) {
  try {
    const auto pair = cursor.current();
    if (*iter == pair)
      return true;

    std::cout << caption << ": expected" << *iter << " got " << pair << "\n";

    auto check = cursor.clone();
    int i;
    for (i = 0; i > -5; --i)
      if (!check.to_previous(false))
        break;

    while (i <= 5) {
      std::cout << ((i < 0) ? "-" : (i) ? "+" : "=") << ((i < 0) ? -i : i) << ") " << check.current() << "\n";
      if (!check.to_next(false))
        break;
      ++i;
    }

    return false;
  } catch (const std::exception &e) {
    std::cout << __func__ << " " << e.what() << "\n";
    return false;
  }
}

MDBX_MAYBE_UNUSED static void checker_erase(verifier &checker, iterator &target) {
#ifndef NDEBUG
  std::cout << "checker-erase " << *target << "\n";
#endif /* NDEBUG */
  checker.erase(target);
}

MDBX_MAYBE_UNUSED static verifier probe_prepare(mdbx::env env, case_kind &kvg, unsigned deep) {
  auto txn = env.start_write();
  txn.clear_map(kvg.table);
  verifier checker(less(kvg.flags));
  for (size_t serial = 0; txn.get_map_stat(kvg.table).ms_depth < deep || (deep && checker.size() < 42); ++serial) {
    auto pair = kvg.kv(serial);
    txn.upsert(kvg.table, pair.key, pair.value);
    checker.insert(std::move(pair));
  }
  txn.commit();
  return checker;
}

//------------------------------------------------------------------------------------------------------------

using case_set = std::vector<std::unique_ptr<case_kind>>;

static bool test_distance(mdbx::env env, case_kind &kvg) {
  auto txn = env.start_read();
  auto forward = txn.open_cursor(kvg.table);

  auto first = forward.clone();
  first.to_first();
  auto last = forward.clone();
  last.to_last();

  unsigned level = 0;
  intptr_t prev_steps = -1, steps;
  intptr_t prev_whole = 0;
  const auto dupsort_deepmask = kvg.is_multivalue() ? txn.get_tree_deepmask(kvg.table) : 0;
  const auto tree_height = txn.get_map_stat(kvg.table).ms_depth;

  while (true) {
    const auto whole_distance = mdbx::cursor::distance_between(first, last, level);
    if (-whole_distance != mdbx::cursor::distance_between(last, first, level))
      return failed(__LINE__);
    if (prev_whole > whole_distance)
      return failed(__LINE__);

    steps = 0;
    forward.assign(first);
    auto backward = last.clone();

    auto prev_forward_turn = forward.clone();
    intptr_t prev_forward_distance = 0;
    auto prev_backward_turn = backward.clone();
    intptr_t prev_backward_distance = 0;

    /* Уже в процессе тестирования и отладки выявился некоторый недостаток задуманного API.
     * При оценке дистанции в dupsort-таблицах результат может зависеть от положения курсоров, поэтому в этом тесте
     * дистанция от first до last, может не совпадать с суммой от first до курсора и от курсора до last.
     *
     * Причина в том, что при учёте dupsort-узлов целиком нет другого рационального решения, кроме как брать точное
     * количество элементов из заголовков этих узлов. В тоже время, при частичном учёте dupsort-узлов (когда целевой
     * курсор стоит где-то внутри вложенного dupsort-дерева), приходится следовать ограничению на глубину погружения в
     * дерево, что изменяет гранулярность и ограничивает точность.
     *
     * Например, когда вычисляется дистанция между first и last, все dupsort-узлы будут посчитаны по их заголовкам с
     * гранулярностью и точностью до каждого элемента. А при расчёте дистанции от fisrt до курсора и от курсора до last,
     * dupsort-дерево на котором стоит такой курсор, будет учитываться с учётом заданного ограничения на глубину,
     * т.е. с гранулярностью и точностью до листовых страниц или целых ветвей этого дерева.
     *
     * Устранять этот недостаток смысла нет:
     *  - сейчас для точности пользователь может увеличить глубину просмотра, при этом затраты возрастут
     *    только на обработку курсоров стоящих внутри вложенных dupsort-деревьев;
     *  - исправление же будет сводиться к обходу всех вложенных dupsort-деревьев только для загрубления
     *    гранулярности и точности в соответствии с заданной ограничением глубины. */
    const bool sum_could_mismatch = level >= tree_height && (dupsort_deepmask >> (level - tree_height)) > 0;

    while (forward.to_next(false) && backward.to_previous(false)) {
      const auto forward_step_distance = forward.distance_from(prev_forward_turn, level);
      if (forward_step_distance < prev_forward_distance)
        return failed(__LINE__);
      if (-forward_step_distance != forward.distance_to(prev_forward_turn, level))
        return failed(__LINE__);

      prev_forward_distance = forward_step_distance;
      if (forward_step_distance > 0) {
        prev_forward_turn.assign(forward);
        prev_forward_distance = 0;
        steps++;
      }

      const auto from_first = mdbx::cursor::distance_between(first, forward, level);
      const auto to_last = mdbx::cursor::distance_between(forward, last, level);
      if (from_first + to_last != whole_distance && !sum_could_mismatch)
        return failed(__LINE__);

      const auto backward_step_distance = backward.distance_from(prev_backward_turn, level);
      if (backward_step_distance > prev_backward_distance)
        return failed(__LINE__);
      if (-backward_step_distance != backward.distance_to(prev_backward_turn, level))
        return failed(__LINE__);

      prev_backward_distance = backward_step_distance;
      if (backward_step_distance < 0) {
        prev_backward_turn.assign(backward);
        prev_backward_distance = 0;
      }

      const auto to_first = mdbx::cursor::distance_between(first, backward, level);
      const auto from_last = mdbx::cursor::distance_between(backward, last, level);
      if (to_first + from_last != whole_distance && !sum_could_mismatch)
        return failed(__LINE__);
    }

    if (prev_steps == steps) {
      if (prev_whole != whole_distance)
        return failed(__LINE__);
      return true;
    }

    if (++level > 9)
      return failed(__LINE__);
    prev_whole = whole_distance;
    prev_steps = steps;
  }
}

static bool probe_scroll(const mdbx::cursor &base, intptr_t scroll, unsigned level, const mdbx::cursor &detent,
                         intptr_t distance, bool distance_linear) {
  try {
    auto cursor = base.clone();
#ifndef NDEBUG
    static unsigned turn_counter;
    if (turn_counter == 999999) {
      mdbx_setup_debug_nofmt(MDBX_LOG_VERBOSE, MDBX_DBG_ASSERT | MDBX_DBG_AUDIT, logger_nofmt, log_buffer,
                             sizeof(log_buffer));
      debug(__LINE__, "got it (%u)", turn_counter);
    }
    debug(__LINE__, "turn: number %u, distance %zi, scroll %zi, level %u, linear %c", ++turn_counter, distance, scroll,
          level, distance_linear ? 'Y' : 'N');
#endif /* NDEBUG */
    cursor.scroll(scroll, level, distance_linear);
    auto delta = cursor.distance_from(base, level);
    if (delta != scroll) {
      if ((delta > 0) != (scroll > 0))
        return failed(__LINE__);
      if ((delta < 0) != (scroll < 0))
        return failed(__LINE__);
      if (distance_linear)
        return failed(__LINE__);
    }
    if (distance_linear) {
      auto after_scroll = cursor.distance_from(detent, level);
      if (after_scroll != distance + scroll)
        return failed(__LINE__);
    }
    return true;
  } catch (const std::exception &e) {
    std::cout << __func__ << " " << e.what() << "\n";
    return false;
  }
}

static bool test_scroll(mdbx::env env, case_kind &kvg) {
  auto txn = env.start_read();
  auto forward = txn.open_cursor(kvg.table);

  auto first = forward.clone();
  first.to_first();
  auto last = forward.clone();
  last.to_last();

  unsigned level = 0;
  intptr_t prev_steps = -1, steps;
  intptr_t prev_whole = 0;
  const auto dupsort_deepmask = kvg.is_multivalue() ? txn.get_tree_deepmask(kvg.table) : 0;
  const auto tree_height = txn.get_map_stat(kvg.table).ms_depth;

  try {
    while (true) {
      const auto whole_distance = mdbx::cursor::distance_between(first, last, level);
      if (prev_whole > whole_distance)
        return failed(__LINE__);

      steps = 0;
      forward.assign(first);
      auto backward = last.clone();

      auto prev_forward_turn = forward.clone();
      intptr_t prev_forward_distance = 0;
      auto prev_backward_turn = backward.clone();
      intptr_t prev_backward_distance = 0;
      const bool sum_could_mismatch = level >= tree_height && (dupsort_deepmask >> (level - tree_height)) > 0;

      do {
        const auto forward_step_distance = forward.distance_from(prev_forward_turn, level);
        if (forward_step_distance < prev_forward_distance)
          return failed(__LINE__);

        prev_forward_distance = forward_step_distance;
        if (forward_step_distance > 0) {
          prev_forward_turn.assign(forward);
          prev_forward_distance = 0;
          steps++;
        }

        const auto from_first = mdbx::cursor::distance_between(first, forward, level);
        const auto to_last = mdbx::cursor::distance_between(forward, last, level);
        if (from_first + to_last != whole_distance && !sum_could_mismatch)
          return failed(__LINE__);

        const auto backward_step_distance = backward.distance_from(prev_backward_turn, level);
        if (backward_step_distance > prev_backward_distance)
          return failed(__LINE__);

        prev_backward_distance = backward_step_distance;
        if (backward_step_distance < 0) {
          prev_backward_turn.assign(backward);
          prev_backward_distance = 0;
        }

        const auto to_first = mdbx::cursor::distance_between(first, backward, level);
        const auto from_last = mdbx::cursor::distance_between(backward, last, level);
        if (to_first + from_last != whole_distance && !sum_could_mismatch)
          return failed(__LINE__);

        for (auto n = std::min(whole_distance, intptr_t(5)); n > 0; --n) {
          if (!probe_scroll(first, prng() % whole_distance, level, first, 0, !sum_could_mismatch))
            return false;
          if (!probe_scroll(last, -intptr_t(prng() % whole_distance), level, last, 0, !sum_could_mismatch))
            return false;

          if (from_first &&
              !probe_scroll(forward, -intptr_t(prng() % from_first), level, first, from_first, !sum_could_mismatch))
            return false;
          if (to_last && !probe_scroll(forward, intptr_t(prng() % to_last), level, last, -to_last, !sum_could_mismatch))
            return false;

          if (to_first &&
              !probe_scroll(backward, -intptr_t(prng() % to_first), level, first, to_first, !sum_could_mismatch))
            return false;
          if (from_last && !probe_scroll(backward, prng() % from_last, level, last, -from_last, !sum_could_mismatch))
            return false;
        }
      } while (forward.to_next(false) && backward.to_previous(false));

      if (prev_steps == steps) {
        if (prev_whole != whole_distance)
          return failed(__LINE__);
        return true;
      }

      if (++level > 9)
        return failed(__LINE__);
      prev_whole = whole_distance;
      prev_steps = steps;
    }
  } catch (const std::exception &e) {
    std::cout << __func__ << " " << e.what() << "\n";
    return false;
  }
}

static bool test_distribute(mdbx::env env, case_kind &kvg) {
  auto txn = env.start_read();

  auto first = txn.open_cursor(kvg.table);
  first.to_first();
  auto last = first.clone();
  last.to_last();

  static const size_t N = 5;
  std::vector<mdbx::cursor_managed> vector;
  vector.reserve(N);
  for (size_t i = 0; i < N; ++i)
    vector.emplace_back(txn.open_cursor(kvg.table));

  unsigned level = 0;
  intptr_t prev_whole = -1;
  const auto dupsort_deepmask = kvg.is_multivalue() ? txn.get_tree_deepmask(kvg.table) : 0;
  const auto tree_height = txn.get_map_stat(kvg.table).ms_depth;

  try {
    while (true) {
      const bool sum_could_mismatch = level >= tree_height && (dupsort_deepmask >> (level - tree_height)) > 0;
      const auto whole_distance = mdbx::cursor::distance_between(first, last, level);
      if (prev_whole > whole_distance)
        return failed(__LINE__);

      auto from = first.clone();
      auto to = last.clone();
      for (intptr_t i = whole_distance / 4; i >= 0; i -= 2) {
#ifndef NDEBUG
        static unsigned turn_counter;
        if (turn_counter == 999999) {
          mdbx_setup_debug_nofmt(MDBX_LOG_VERBOSE, MDBX_DBG_ASSERT | MDBX_DBG_AUDIT, logger_nofmt, log_buffer,
                                 sizeof(log_buffer));
          debug(__LINE__, "got it (%u)", turn_counter);
        }
        debug(__LINE__, "turn: number %u, distance %zi, level %u", ++turn_counter, whole_distance, level);
#endif /* NDEBUG */
        mdbx::cursor::distribute(from, to, vector, level);
        mdbx::cursor prev = from;
        intptr_t prev_dist = -1;
        for (auto &cursor : vector) {
          if (!cursor.eof()) {
            auto dist = cursor.distance_from(prev, level);
            auto to_last = cursor.distance_to(to, level);
            if (dist < 0)
              return failed(__LINE__);
            if (to_last < 0)
              return failed(__LINE__);
            if (prev_dist >= 0) {
              if (std::abs(prev_dist - dist) > 1 && !sum_could_mismatch)
                return failed(__LINE__);
            }
            prev = cursor;
            prev_dist = dist;
          }
        }
        if (!from.to_next(false) || !to.to_previous(false) || mdbx::cursor::distance_between(from, to, level) < 0)
          break;
      }

      if (prev_whole == whole_distance)
        return true;
      if (++level > 9)
        return failed(__LINE__);
      prev_whole = whole_distance;
    }
  } catch (const std::exception &e) {
    std::cout << __func__ << " " << e.what() << "\n";
    return false;
  }
}

static bool test(mdbx::env env, case_set &set, unsigned deep) {
  bool ok = true;
  for (auto &kvg : set) {
    std::cout << kvg->name << ": " << " deep " << deep << std::endl;
    auto checker = probe_prepare(env, *kvg, deep);
    ok = test_distance(env, *kvg) && ok;
    ok = test_scroll(env, *kvg) && ok;
    ok = test_distribute(env, *kvg) && ok;
  }
  return ok;
}

//------------------------------------------------------------------------------------------------------------

int doit() {
#ifdef NDEBUG
  std::random_device random;
  std::seed_seq seed({random(), random(), random(), random(), random()});
#else
  std::seed_seq seed({42});
#endif

  std::cout << "seed ";
  seed.param(std::ostream_iterator<size_t>(std::cout, ", "));
  std::cout << std::endl;
  prng.seed(seed);

  mdbx::path db_filename = "test-distance-scroll";
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
  mdbx_setup_debug_nofmt(MDBX_LOG_NOTICE /* MDBX_LOG_VERBOSE */, MDBX_DBG_ASSERT | MDBX_DBG_AUDIT, logger_nofmt,
                         log_buffer, sizeof(log_buffer));
  try {
    return doit();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
