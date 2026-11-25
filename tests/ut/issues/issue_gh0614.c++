/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement. In no event shall the
/// authors or copyright holders be liable for any claim, damages or other liability, whether in an action of contract,
/// tort or otherwise, arising from, out of or in connection with the software or the use or other dealings in the
/// software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/614
//
// A defragmentation cycle that forms a run for a large page schedules the pages that occupy the window of the run one
// by one. When one of them gets no place, the pages scheduled before stay in the cycle and are moved. The next cycle
// finds another window the cheapest one and moves them back. So the cycles went on without end, a few pages moving
// from one place to another and back, while the large page at the end of the data never moved: the copies of its
// parents got no place below the goal edge.
//
// The guards of mdbx_env_defrag() did not notice it. A retry was counted only when the walk stumbled at the page of the
// previous cycle again, and it stumbled at different pages in turn; a cycle without moves was counted only among
// cycles without moves in a row, and every second cycle moved pages. Now the defragmentation remembers the best state
// of the trees the cycles started from: a cycle that starts from no better state is no progress, except one that
// reserved a run of free pages for a large page and so prepared the move of it. The effect of a cycle shows at the
// start of the next one, so the second such cycle in a row may still bring that of the first, and the third one ends
// the call: the pages the cycles stumbled on tell a limit of the trees from an obstacle, and the places between the
// goal edge and the pages cure the walk as they do for one cycle.
//
// The first two tests are the workload of the jitter testcase of the stochastic test as one deterministic line: one
// table of the smallest pages, a record put or deleted in every transaction, and a defragmentation after every second
// one. A record has a key of 70 to 100 bytes and a value of 600 to 1100 bytes. The seed is the one of the shortest
// sequence found: the database has 59 pages and a large page of three pages at the end when the defragmentation used
// to repeat four cycles without end, with the root page of the table going from the page 3 to the page 12 and back.
// The tests run the first 80 transactions of the sequence: the defragmentation that used to repeat its cycles is the
// one after the transaction 39. The whole sequence has 343 transactions and 172 defragmentations, and in the cells of
// the CI under Windows the binary then took from 4 to 12 seconds against a timeout of 30. The first test defragments
// as the testcase does; the second one as a tool with a limit of time does, with the batch left to the library, which
// then changes it from cycle to cycle: the walk still comes back to the same states, and the call must end when it
// does.
//
// The third test needs no particular layout. A batch of one page makes a cycle of every page that moves, so one call
// needs about 130 cycles. Every cycle moves pages closer to the beginning, so every one of them makes progress: the
// call must go on to its goal, and the stall detection must not stop it on the way.

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include "../test-env.h++"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *const kPath = "issue0614.mdbx";
const char *const kTable = "t0";
/* The seed of the sequence of the first two tests. */
constexpr uint64_t kSeed = 13959;
/* The transactions of the sequence the first two tests run. */
constexpr unsigned kTransactions = 80;
/* The records of the third test: one to a page, every second one deleted. The call then needs about 130 cycles. */
constexpr uint32_t kSpacedRecords = 600;
/* The defragmentation is stopped when a call goes beyond this number of cycles; the longest call of the tests needs
 * eleven. */
constexpr unsigned kCycleLimit = 1000;
/* The limit of time of the test that defragments as a tool: one second in the 16.16 format, stretched on the slower
 * setups as the test timeouts are, for the call must not reach it there either. The defragmentation of the sequence
 * needs milliseconds. */
size_t time_limit_dot16() { return size_t(mdbx_tst_scaled_wait_seconds(1)) << 16; }

/* The generator of the sequence: the same numbers for the same seed on every platform. */
uint64_t rng_state;

uint64_t rnd() {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1Dull;
}

unsigned rnd_range(unsigned lo, unsigned hi) {
  return lo + static_cast<unsigned>(rnd() % static_cast<uint64_t>(hi - lo + 1));
}

struct record {
  unsigned char key[100];
  unsigned length;
  std::string value;
};

std::vector<record> records;
unsigned longest;
bool stopped_by_the_test;
bool time_limit_reached;

/* How the tests defragment: no goal, no limit of time and the batch left unlimited, as the jitter testcase does; or
 * under a limit of time with the batch of the autopilot, which the library changes from cycle to cycle. */
enum defrag_way { defrag_as_the_testcase, defrag_as_a_tool };

int on_progress(void *, const MDBX_defrag_result_t *progress) noexcept {
  if (progress->cycles > longest)
    longest = progress->cycles;
  time_limit_reached |= (progress->stopping_reasons & MDBX_defrag_time_limit) != 0;
  if (progress->cycles > kCycleLimit)
    stopped_by_the_test = true;
  return stopped_by_the_test ? -1 : 0;
}

int defrag(MDBX_env *env, defrag_way way) {
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  if (way == defrag_as_a_tool)
    return mdbx_env_defrag(env, 0, 0, 0, time_limit_dot16(), -1, -1, on_progress, nullptr, &result);
  return mdbx_env_defrag(env, 0, 0, 0, 0, -1, 0, on_progress, nullptr, &result);
}

/* EXPECT_EQ() and the answer, for the places where an assertion macro can not return. */
bool ok(int rc) {
  EXPECT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);
  return rc == MDBX_SUCCESS;
}

/* Commits a transaction of the guard's scope. The guard gives the handle away right before the commit, since the
 * commit ends the transaction both ways: it frees the handle on success and aborts it on failure. */
bool commit(MDBX_txn *txn, txn_handle_guard &owner) {
  owner.release();
  return ok(mdbx_txn_commit(txn));
}

/* The environment of the stochastic test on the database `path`: the smallest pages and the mode of the test. */
MDBX_env *open_environment(const char *path) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  mdbx_env_set_maxdbs(env, 8);
  mdbx_env_set_maxreaders(env, 42);
  /* The sequences need a few hundred kilobytes. The upper limit is kept far below the limit of the mapping, which
   * under the sanitizers is bounded by the memory of the machine: a larger one is refused there. */
  const intptr_t upper = static_cast<intptr_t>(64) << 20;
  const MDBX_env_flags_t mode =
      MDBX_UTTERLY_NOSYNC | MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS | MDBX_LIFORECLAIM | MDBX_ACCEDE;
  if (mdbx_env_set_geometry(env, -1, -1, upper, -1, -1, mdbx_limits_pgsize_min()) != MDBX_SUCCESS ||
      mdbx_env_open(env, path, mode, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

/* The table of the tests in a fresh database. */
bool open_table(MDBX_env *env, MDBX_dbi *dbi) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (!ok(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)))
    return false;
  if (!ok(mdbx_dbi_open(txn, kTable, MDBX_CREATE, dbi)))
    return false;
  return commit(txn, owner);
}

/* The number of the records of the table. */
bool count_records(MDBX_env *env, MDBX_dbi dbi, size_t *count) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (!ok(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn)))
    return false;
  MDBX_stat stat;
  memset(&stat, 0, sizeof(stat));
  const bool stated = ok(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)));
  *count = static_cast<size_t>(stat.ms_entries);
  return stated;
}

bool put(MDBX_env *env, MDBX_dbi dbi, uint64_t tag) {
  record rec;
  rec.length = rnd_range(70, 100);
  const unsigned value_bytes = rnd_range(600, 1100);
  for (unsigned j = 0; j < rec.length; ++j)
    rec.key[j] = static_cast<unsigned char>(rnd());
  /* The first nine bytes of the key are the number of the record and the number of its table, zero here: so the keys
   * of the table stay unique. */
  memcpy(rec.key, &tag, 8);
  rec.key[8] = 0;
  rec.value.resize(value_bytes);
  for (unsigned j = 0; j < value_bytes; ++j)
    rec.value[j] = static_cast<char>(rnd());

  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (!ok(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)))
    return false;
  MDBX_val k = {rec.key, rec.length}, v = {rec.value.data(), rec.value.size()};
  if (!ok(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT)) || !commit(txn, owner))
    return false;
  records.push_back(std::move(rec));
  return true;
}

bool del(MDBX_env *env, MDBX_dbi dbi) {
  const size_t index = rnd() % records.size();
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (!ok(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)))
    return false;
  MDBX_val k = {records[index].key, records[index].length};
  if (!ok(mdbx_del(txn, dbi, &k, nullptr)) || !commit(txn, owner))
    return false;
  records[index] = std::move(records.back());
  records.pop_back();
  return true;
}

/* The first transactions of the sequence of the jitter testcase from its seed, with a defragmentation after every
 * second one. Gives whether every step went well; the answer about the defragmentations is in the counters of the
 * callback. */
bool run_sequence(MDBX_env *env, MDBX_dbi dbi, defrag_way way) {
  rng_state = kSeed * 0x9E3779B97F4A7C15ull + 0x123456789ABCDEFull;
  const unsigned tables = rnd_range(1, 4);
  /* The seed gives one table: the test has no use for the others. */
  EXPECT_EQ(1u, tables);
  if (tables != 1)
    return false;
  const unsigned steps = rnd_range(300, 900), del_percent = rnd_range(20, 60);
  /* The first and the last transaction of a reading transaction of the sequence. The numbers are drawn to keep the
   * sequence of the seed; the tests stop long before the transaction opens. */
  (void)rnd_range(0, steps);
  (void)rnd_range(20, 200);
  EXPECT_GT(steps, kTransactions) << "the seed gives a shorter sequence than the tests run";
  uint64_t tag = 0;
  longest = 0;
  stopped_by_the_test = false;
  time_limit_reached = false;
  records.clear();

  for (unsigned step = 1; step <= kTransactions; ++step) {
    /* The table of the operation: the only one, so the number is not kept. */
    (void)rnd_range(0, tables - 1);
    const bool done = (!records.empty() && rnd() % 100 < del_percent) ? del(env, dbi) : put(env, dbi, ++tag);
    if (!done)
      return false;
    if (step & 1) {
      const int rc = defrag(env, way);
      if (stopped_by_the_test) {
        ADD_FAILURE() << "step " << step << ": the defragmentation went beyond " << kCycleLimit << " cycles";
        return false;
      }
      EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE)
          << "step " << step << ": rc " << rc << " (" << mdbx_strerror(rc) << ")";
    }
  }
  return true;
}

/* Every record of the table is read back and compared with what was put. */
bool verify_records(MDBX_env *env, MDBX_dbi dbi) {
  size_t count = 0;
  if (!count_records(env, dbi, &count))
    return false;
  EXPECT_EQ(records.size(), count);
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (!ok(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn)))
    return false;
  for (record &rec : records) {
    MDBX_val k = {rec.key, rec.length}, v;
    const int rc = mdbx_get(txn, dbi, &k, &v);
    EXPECT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);
    if (rc == MDBX_SUCCESS) {
      EXPECT_EQ(rec.value.size(), v.iov_len);
      if (v.iov_len == rec.value.size()) {
        EXPECT_EQ(0, memcmp(rec.value.data(), v.iov_base, v.iov_len));
      }
    }
  }
  return true;
}

} // namespace

TEST(issue_gh614, defragmentation_does_not_repeat_its_cycles) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  MDBX_env *env = open_environment(kPath);
  env_handle_guard env_owner(&env);
  ASSERT_NE(nullptr, env);
  MDBX_dbi dbi;
  ASSERT_TRUE(open_table(env, &dbi));

  ASSERT_TRUE(run_sequence(env, dbi, defrag_as_the_testcase));
  EXPECT_LT(longest, 40u) << " cycles in the longest call";
  ASSERT_TRUE(verify_records(env, dbi));
}

TEST(issue_gh614, defragmentation_under_a_time_limit_does_not_repeat_its_cycles) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  /* The oscillation of the sequence used to go on until the limit of time, cycle by cycle, although the batch was
   * left to the library. */
  MDBX_env *env = open_environment(kPath);
  env_handle_guard env_owner(&env);
  ASSERT_NE(nullptr, env);
  MDBX_dbi dbi;
  ASSERT_TRUE(open_table(env, &dbi));

  ASSERT_TRUE(run_sequence(env, dbi, defrag_as_a_tool));
  EXPECT_LT(longest, 40u) << " cycles in the longest call";
  EXPECT_FALSE(time_limit_reached) << " a call went on until the limit of time";
  ASSERT_TRUE(verify_records(env, dbi));
}

TEST(issue_gh614, hundreds_of_cycles_in_one_call) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  MDBX_env *env = open_environment(kPath);
  env_handle_guard env_owner(&env);
  ASSERT_NE(nullptr, env);
  MDBX_dbi dbi;
  ASSERT_TRUE(open_table(env, &dbi));

  /* A record of half a page: one record to a page. Every second one is deleted. */
  MDBX_envinfo info;
  memset(&info, 0, sizeof(info));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  const std::string value(info.mi_dxb_pagesize / 2, 'v');
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    for (uint32_t i = 0; i < kSpacedRecords; ++i) {
      MDBX_val k = {&i, sizeof(i)}, v = {const_cast<char *>(value.data()), value.size()};
      if (!ok(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT)))
        return;
    }
    for (uint32_t i = 0; i < kSpacedRecords; i += 2) {
      MDBX_val k = {&i, sizeof(i)};
      if (!ok(mdbx_del(txn, dbi, &k, nullptr)))
        return;
    }
    if (!commit(txn, owner))
      return;
  }

  /* The batch of one page makes a cycle of every page that moves. */
  longest = 0;
  stopped_by_the_test = false;
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 1, on_progress, nullptr, &result);
  EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) << "rc " << rc << " (" << mdbx_strerror(rc) << ")";
  EXPECT_FALSE(stopped_by_the_test) << "the defragmentation went beyond " << kCycleLimit << " cycles";
  EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error);
  /* The call goes on for many cycles, each of them moving a page or two: no stall, since every cycle leaves the trees
   * better than the best state before it. */
  EXPECT_LT(64u, result.cycles);

  /* Every record left is read back and compared with what was put. */
  size_t count = 0;
  ASSERT_TRUE(count_records(env, dbi, &count));
  EXPECT_EQ(kSpacedRecords / 2, static_cast<uint32_t>(count));
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  for (uint32_t i = 1; i < kSpacedRecords; i += 2) {
    uint32_t key = i;
    MDBX_val k = {&key, sizeof(key)}, v;
    const int got = mdbx_get(txn, dbi, &k, &v);
    EXPECT_EQ(MDBX_SUCCESS, got) << mdbx_strerror(got);
    if (got == MDBX_SUCCESS) {
      EXPECT_EQ(value.size(), v.iov_len);
      if (v.iov_len == value.size()) {
        EXPECT_EQ(0, memcmp(value.data(), v.iov_base, v.iov_len));
      }
    }
  }
}
