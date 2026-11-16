/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* mdbx_env_defrag() API contracts, complementary to defrag-cycle (metrics of
 * a full cycle), defrag-cursors (the discontinue path) and issue gh0306
 * (adopted-txn data persistence):
 *
 *  - the argument-validation matrix (MDBX_EINVAL);
 *  - the refusal inside a nested write transaction (MDBX_BAD_TXN, the #306
 *    semantics: a basal write txn is adopted, a nested one is refused);
 *  - the completion report: rc == MDBX_SUCCESS with a sane result
 *    (pages_moved > 0, pages_whole > 0, permille <= 1000, cycles >= 1) and
 *    the progress callback firing at least once per begun cycle;
 *  - the time-limit stop through the adaptive-batch path
 *    (time_limit set, preferred_batch = 0).
 *
 * Known contract gaps of the verification campaign (#485) are pinned with
 * `known contract gap V-0xx` comments instead of the documented wording:
 * V-060 (a stuck-retry stop still reports SUCCESS + noobstacles), V-061
 * (time_limit is only evaluated once the defrag_atleast goal edge is
 * reached - here atleast=0 keeps the gate open, so the limit applies),
 * V-064 (the adopted-txn per-cycle checkpoint commits the user's pending
 * writes mid-transaction and detaches the user's cursors). */

namespace {

struct progress_probe {
  unsigned calls = 0;
  static int on_progress(void *ctx, const MDBX_defrag_result_t *) noexcept {
    static_cast<progress_probe *>(ctx)->calls += 1;
    return 0; /* continue */
  }
};

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

struct txn_ptr {
  MDBX_txn *txn = nullptr;
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

void churn_fragment(const char *dbname, const char *table) {
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), dbname, MDBX_NOSUBDIR, 0644));
  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, table, MDBX_CREATE, &dbi));
    char keybuf[16], valbuf[192];
    for (uint32_t i = 0; i < 6000; ++i) {
      std::memset(keybuf, 0, sizeof keybuf);
      std::memcpy(keybuf, &i, sizeof i);
      MDBX_val key{keybuf, sizeof keybuf}, val{valbuf, sizeof valbuf};
      for (size_t j = 0; j < sizeof valbuf; ++j)
        valbuf[j] = static_cast<char>('a' + (i + j) % 26);
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, dbi, &key, &val, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }
  {
    /* a single delete pass of every second record - the proven recipe from
     * defrag-cycle that leaves enough retired pages for real defrag cycles */
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, table, MDBX_DB_DEFAULTS, &dbi));
    char keybuf[16];
    for (uint32_t i = 0; i < 6000; i += 2) {
      std::memset(keybuf, 0, sizeof keybuf);
      std::memcpy(keybuf, &i, sizeof i);
      MDBX_val key{keybuf, sizeof keybuf};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_del(txn.txn, dbi, &key, nullptr));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }
  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, table, MDBX_DB_DEFAULTS, &dbi));
    char keybuf[16], valbuf[160];
    for (uint32_t i = 0; i < 4000; ++i) {
      const uint32_t k = i + 0x10000u;
      std::memset(keybuf, 0, sizeof keybuf);
      std::memcpy(keybuf, &k, sizeof k);
      MDBX_val key{keybuf, sizeof keybuf}, val{valbuf, sizeof valbuf};
      for (size_t j = 0; j < sizeof valbuf; ++j)
        valbuf[j] = static_cast<char>('A' + (i + j) % 26);
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, dbi, &key, &val, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }
}

} // namespace

TEST(defrag_contract, argument_validation) {
  const env_files_guard guard({"defrag-contract-validate.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *kDb = "defrag-contract-validate.mdbx";
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));
  MDBX_defrag_result_t result;

  /* defrag_enough below defrag_atleast (and not zero) is refused */
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_defrag(env.get(), 16, 0, 4, 0, 0, 0, nullptr, nullptr, &result));
  /* time_limit below time_atleast (and not zero) is refused */
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_defrag(env.get(), 0, 65536, 0, 2, 0, 0, nullptr, nullptr, &result));
  /* a notification context without the callback is refused */
  int dummy = 0;
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_defrag(env.get(), 0, 0, 0, 0, 0, 0, nullptr, &dummy, &result));
}

TEST(defrag_contract, nested_write_txn_refused) {
  const env_files_guard guard({"defrag-contract-nested.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *kDb = "defrag-contract-nested.mdbx";
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));
  txn_ptr parent;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &parent));
  txn_ptr nested;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), parent.txn, MDBX_TXN_READWRITE, &nested));
  MDBX_defrag_result_t result;
  /* the #306 semantics: the basal write transaction is adopted, but a
   * nested one cannot be - refuse with MDBX_BAD_TXN and keep both usable */
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_env_defrag(env.get(), 1, 0, 16, 0, 0, 4096, nullptr, nullptr, &result));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(nested.txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
  MDBX_val key = {(void *)"k", 1}, value = {(void *)"v", 1};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_put(nested.txn, dbi, &key, &value, MDBX_UPSERT));
}

TEST(defrag_contract, completion_reports_success) {
  const env_files_guard guard({"defrag-contract-cycle.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *kDb = "defrag-contract-cycle.mdbx";
  churn_fragment(kDb, "main");

  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  progress_probe probe;
  MDBX_defrag_result_t result;
  std::memset(&result, 0, sizeof result);
  const int rc = mdbx_env_defrag(env.get(), 1, 0, 16, 0, 0, 4096, progress_probe::on_progress, &probe, &result);
  ASSERT_EQ(MDBX_SUCCESS, rc);
  EXPECT_GT(result.pages_moved, 0u);
  EXPECT_GT(result.pages_whole, 0u);
  EXPECT_LE(result.rough_estimation_cycle_progress_permille, 1000u);
  EXPECT_GE(result.cycles, 1u);
  EXPECT_GE(probe.calls, result.cycles) << "the callback fires at the beginning and the end of every cycle";
  /* known contract gap V-060: a stuck-retry stop would also report
   * MDBX_SUCCESS with stopping_reasons == noobstacles despite the named
   * obstacle - not driven here (nondeterministic to construct). */

  /* the data is intact after the defrag cycles */
  txn_ptr txn;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "main", MDBX_DB_DEFAULTS, &dbi));
  MDBX_stat stat;
  std::memset(&stat, 0, sizeof stat);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn.txn, dbi, &stat, sizeof stat));
  EXPECT_EQ(stat.ms_entries, 6000u - 3000u + 4000u);
}

TEST(defrag_contract, time_limit_stop_with_adaptive_batch) {
  const env_files_guard guard({"defrag-contract-time.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *kDb = "defrag-contract-time.mdbx";
  churn_fragment(kDb, "main");

  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  MDBX_defrag_result_t result;
  std::memset(&result, 0, sizeof result);
  /* time_limit set with preferred_batch = 0 selects the adaptive batch
   * (the cycle size aims at a fraction of the remaining time budget).
   * With defrag_atleast = 0 the goal edge is pre-reached, so the limit
   * gate is open.
   * known contract gap V-061: with an unreachable defrag_atleast goal the
   * time limit would never be evaluated (defrag_progress gating) - that
   * side is deliberately not asserted. */
  const int rc = mdbx_env_defrag(env.get(), 0, 0, 0, 65536 / 4, 0, 0, nullptr, nullptr, &result);
  ASSERT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) << "rc=" << rc;
  EXPECT_GE(result.cycles, 1u);
  EXPECT_LE(result.rough_estimation_cycle_progress_permille, 1000u);
  if (rc == MDBX_RESULT_TRUE) {
    EXPECT_TRUE(result.stopping_reasons & MDBX_defrag_time_limit)
        << "stopping with work left must name the time limit, got " << result.stopping_reasons;
  }
  /* known contract gap V-061: the docs promise an unconditional time limit. */
}

TEST(defrag_contract, adopted_txn_checkpoint_commits_and_detaches) {
  const env_files_guard guard({"defrag-contract-adopt.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *kDb = "defrag-contract-adopt.mdbx";
  churn_fragment(kDb, "main");

  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  txn_ptr txn;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn.txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "main", MDBX_DB_DEFAULTS, &dbi));
  MDBX_val key = {(void *)"pending-key", 11}, value = {(void *)"pending-value", 13};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, dbi, &key, &value, MDBX_UPSERT));
  MDBX_cursor *mc = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &mc));
  MDBX_val k = {(void *)"any", 3}, d;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(mc, &k, &d, MDBX_SET_RANGE));

  MDBX_defrag_result_t result;
  std::memset(&result, 0, sizeof result);
  const int rc = mdbx_env_defrag(env.get(), 1, 0, 16, 0, 0, 4096, nullptr, nullptr, &result);
  ASSERT_EQ(MDBX_SUCCESS, rc);

  /* known contract gap V-064: the per-cycle checkpoint committed the user's
   * pending write mid-transaction and detached the user's cursors: any
   * further cursor operation is refused with MDBX_EINVAL ... */
  EXPECT_EQ(MDBX_EINVAL, mdbx_cursor_get(mc, &k, &d, MDBX_GET_CURRENT));
  mdbx_cursor_close(mc);
  mdbx_txn_abort(txn.release());
  {
    txn_ptr check;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &check.txn));
    MDBX_dbi rdbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(check.txn, "main", MDBX_DB_DEFAULTS, &rdbi));
    MDBX_val got;
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get(check.txn, rdbi, &key, &got))
        << "the pending write survives the abort: committed by the defrag checkpoint";
  }
}

TEST(defrag_contract, stuck_retry_reports_success) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({"defrag-contract-stuck.mdbx"});
  const char *kDb = "defrag-contract-stuck.mdbx";
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env.get(), -1, 1024 * 1024, 64 * 1024 * 1024, -1, -1, 4096));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  auto put_key = [](MDBX_txn *txn, MDBX_dbi dbi, unsigned k, unsigned seed) {
    char keybuf[16], valbuf[256];
    const unsigned n = 8 + (seed % 200);
    MDBX_val key = {keybuf, (size_t)std::snprintf(keybuf, sizeof keybuf, "k-%08u", k)};
    std::memset(valbuf, (int)(k & 0xFF), sizeof valbuf);
    MDBX_val val = {valbuf, n};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT));
  };
  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi main_dbi, named_dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "named", MDBX_CREATE, &named_dbi));
    for (unsigned i = 0; i < 12000; ++i)
      put_key(txn.txn, main_dbi, i, i * 31 + 7);
    for (unsigned i = 0; i < 12000; ++i)
      put_key(txn.txn, named_dbi, i, i * 17 + 3);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }
  { /* fragment: delete every other key in both tables */
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi main_dbi, named_dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "named", MDBX_DB_DEFAULTS, &named_dbi));
    for (unsigned i = 0; i < 12000; i += 2) {
      char keybuf[16];
      MDBX_val key = {keybuf, (size_t)std::snprintf(keybuf, sizeof keybuf, "k-%08u", i)};
      EXPECT_EQ(MDBX_SUCCESS, mdbx_del(txn.txn, main_dbi, &key, nullptr));
      EXPECT_EQ(MDBX_SUCCESS, mdbx_del(txn.txn, named_dbi, &key, nullptr));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }

  /* the campaign repro shape: no user txn, a tight backlash and a small
   * preferred_batch force many small cycles; on the 4 KiB reference the
   * defrag gets STUCK retrying the same obstacle page and stops */
  MDBX_defrag_result_t result;
  std::memset(&result, 0, sizeof result);
  const int rc = mdbx_env_defrag(env.get(), 0, 0, 0, 0, 4, 400, nullptr, nullptr, &result);
  ASSERT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) << "rc=" << rc;
  EXPECT_GE(result.cycles, 1u);
  EXPECT_GT(result.pages_moved, 0u);
  EXPECT_LE(result.rough_estimation_cycle_progress_permille, 1000u);

  if (result.obstructed_pgno != 0) {
    /* the stuck-retry stop (V-060): rc may be SUCCESS with
     * stopping_reasons == noobstacles(0) despite the named obstacle -
     * the residual work is visible only via pages_left / obstructed_*
     * (known contract gap V-060 from issue #485, fix/docs pending) */
    EXPECT_EQ(MDBX_defrag_noobstacles, result.stopping_reasons)
        << "known contract gap V-060: the stuck stop reports no obstacles";
    EXPECT_EQ(MDBX_SUCCESS, rc) << "the enough-goal override masks the stuck stop";
  } else {
    EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error);
  }

  /* the data is intact after the multi-cycle defrag */
  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi named_dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "named", MDBX_DB_DEFAULTS, &named_dbi));
    MDBX_stat stat;
    std::memset(&stat, 0, sizeof stat);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn.txn, named_dbi, &stat, sizeof stat));
    EXPECT_EQ(6000u, stat.ms_entries);
    MDBX_cursor *cur = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, named_dbi, &cur));
    MDBX_val key, data;
    unsigned count = 0;
    int crc = mdbx_cursor_get(cur, &key, &data, MDBX_FIRST);
    while (crc == MDBX_SUCCESS) {
      ++count;
      ASSERT_LT(count, 6001u);
      EXPECT_EQ(0, std::memcmp(key.iov_base, "k-", 2));
      crc = mdbx_cursor_get(cur, &key, &data, MDBX_NEXT);
    }
    mdbx_cursor_close(cur);
    EXPECT_EQ(MDBX_NOTFOUND, crc);
    EXPECT_EQ(6000u, count);
  }
}
