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
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/314
// When an mdbx_env_chk() callback returned an error from `scope_push` (as
// mdbx.h allows: "an error code that will be propagated"), mdbx_env_chk()
// still called chk_scope_end() for the scope that was never entered. At depth
// 0 only an ASSERT guarded it, so a release build wrapped `scope_depth` from 0
// to 255, wrote (void *)-1 into the internal table array, and chk_dispose()
// freed it: heap corruption on Windows, SIGSEGV on Linux. At a verbosity above
// MDBX_chk_info the same error made the per-table report use the null scope
// that chk_scope_push() returns, and a `stage_begin` error with a `stage_end`
// that returns 0 (also allowed) fired an assertion in debug builds.
// Here every `scope_push` call, and separately every `stage_begin` call, of a
// clean run is made to fail in turn, at two verbosity levels; every entered
// scope must be left exactly once.

namespace {

const char *const kDb = "issue314.mdbx";

void remove_db() {
  std::remove(kDb);
  std::remove((std::string(kDb) + "-lck").c_str());
}

/* Deletes the database before the test and on every exit path of it. */
struct db_remover {
  db_remover() { remove_db(); }
  ~db_remover() { remove_db(); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

/* The failing callback counts its calls and fails the `fail_at`-th one (1-based; 0 never fails). */
int calls, fail_at, entered, left;
bool fail_stage_begin;
/* The `refuse_at`-th `scope_push` call of the `refuse_stage` stage (or of any
 * stage when MDBX_chk_none) declines entering: returns MDBX_RESULT_TRUE. */
int refuse_at;
enum MDBX_chk_stage refuse_stage = MDBX_chk_none;
/* When true, the `issue` callback counts the phantom mismatch issues and
 * `last_total_problems` captures the result totals of the last run. */
bool count_mismatches, ignore_stage_errors;
int mismatch_issues;
size_t last_total_problems;
enum MDBX_chk_stage failed_stage;

int scope_push(MDBX_chk_context_t *, MDBX_chk_scope_t *, MDBX_chk_scope_t *inner, const char *, va_list) {
  if (!fail_stage_begin) {
    /* when a stage_begin failure is armed, the shared counter is driven by
     * the stage_begin callback instead (see the gh0314 regression scenario) */
    ++calls;
    if (calls == fail_at)
      return MDBX_EINTR;
    if ((refuse_stage != MDBX_chk_none) ? inner->stage == refuse_stage : calls == refuse_at)
      return MDBX_RESULT_TRUE /* declines entering the scope, counted as a problem */;
  }
  ++entered;
  return MDBX_SUCCESS;
}

void scope_pop(MDBX_chk_context_t *, MDBX_chk_scope_t *, MDBX_chk_scope_t *) { ++left; }

int stage_begin(MDBX_chk_context_t *, enum MDBX_chk_stage stage) {
  if (!fail_stage_begin || ++calls != fail_at)
    return MDBX_SUCCESS;
  failed_stage = stage;
  return MDBX_EINTR;
}

/* Ignores the error of the stage whose `stage_begin` failed, which mdbx.h allows too. */
int stage_end(MDBX_chk_context_t *, enum MDBX_chk_stage stage, int err) {
  return (ignore_stage_errors || stage == failed_stage) ? MDBX_SUCCESS : err;
}

/* Counts the phantom mismatch issues: the checks that compare incomplete
 * page-accounting and must not run without the data of a declined scope. */
void issue_cb(MDBX_chk_context_t *, const char *object, uint64_t, const char *caption, const char *, va_list) {
  (void)object;
  if (count_mismatches && caption && (!strcmp(caption, "used pages mismatch") || !strcmp(caption, "GC pages mismatch")))
    ++mismatch_issues;
}

int run_chk_full(MDBX_env *env, MDBX_chk_severity_t verbosity, MDBX_chk_context_t *ctx_out, int fail, int refuse,
                 enum MDBX_chk_stage refuse_st, bool ignore_errors, bool count_mism, bool stage_begin_failing) {
  MDBX_chk_callbacks_t cb;
  MDBX_chk_context_t ctx;
  std::memset(&cb, 0, sizeof(cb));
  std::memset(&ctx, 0, sizeof(ctx));
  cb.scope_push = scope_push;
  cb.scope_pop = scope_pop;
  cb.stage_begin = stage_begin;
  cb.stage_end = stage_end;
  if (count_mism)
    cb.issue = issue_cb;
  calls = entered = left = mismatch_issues = 0;
  fail_at = fail;
  refuse_at = refuse;
  refuse_stage = refuse_st;
  failed_stage = MDBX_chk_none;
  ignore_stage_errors = ignore_errors;
  count_mismatches = count_mism;
  fail_stage_begin = stage_begin_failing;
  const int rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, verbosity, 0);
  last_total_problems = ctx.result.total_problems;
  if (ctx_out)
    *ctx_out = ctx;
  return rc;
}

int run_chk(MDBX_env *env, MDBX_chk_severity_t verbosity, int at) {
  return run_chk_full(env, verbosity, nullptr, at, 0, MDBX_chk_none, false, false, fail_stage_begin);
}

} // namespace

TEST(issue_gh0314, chk_survives_a_failing_callback) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = 0;
    MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
    const int rc = mdbx_dbi_open(txn, "table", MDBX_CREATE, &dbi);
    if (rc != MDBX_SUCCESS || mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      FAIL() << "could not fill the database";
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  for (const MDBX_chk_severity_t verbosity : {MDBX_chk_result, MDBX_chk_extra}) {
    for (const bool stage : {false, true}) {
      fail_stage_begin = stage;
      const char *const what = stage ? "stage_begin" : "scope_push";
      ASSERT_EQ(MDBX_SUCCESS, run_chk(env, verbosity, 0))
          << "a clean check, counting the " << what << " calls, verbosity " << verbosity;
      const int total = calls;
      ASSERT_GT(total, 1) << what;
      EXPECT_EQ(entered, left) << "a clean check, verbosity " << verbosity;
      /* the first call is the issue's case: the check stops and returns the callback's error */
      EXPECT_EQ(MDBX_EINTR, run_chk(env, verbosity, 1)) << what << ", verbosity " << verbosity;
      /* the error must be counted as a problem, not silently lost */
      EXPECT_GE(last_total_problems, 1) << what << ", verbosity " << verbosity;
      /* a later call must not crash or corrupt the heap either; the check may go on or stop */
      for (int at = 1; at <= total; ++at) {
        run_chk(env, verbosity, at);
        EXPECT_EQ(entered, left) << what << " #" << at << " failed, verbosity " << verbosity;
      }
    }
  }
}

/* Declining (MDBX_RESULT_TRUE from scope_push) any of the scopes, in turn:
 * mdbx_env_chk() must report MDBX_PROBLEM, account the refusal as a problem,
 * keep the entered/left scopes balanced and produce no phantom issues. */
TEST(issue_gh0314, chk_accounts_a_declined_scope) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = 0;
    MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
    const int rc = mdbx_dbi_open(txn, "table", MDBX_CREATE, &dbi);
    if (rc != MDBX_SUCCESS || mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      FAIL() << "could not fill the database";
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  for (const MDBX_chk_severity_t verbosity : {MDBX_chk_result, MDBX_chk_extra}) {
    ASSERT_EQ(MDBX_SUCCESS, run_chk_full(env, verbosity, nullptr, 0, 0, MDBX_chk_none, false, false, false))
        << "a clean check, verbosity " << verbosity;
    const int total = calls;
    ASSERT_GT(total, 1) << "verbosity " << verbosity;
    for (int at = 1; at <= total; ++at) {
      const int rc = run_chk_full(env, verbosity, nullptr, 0, at, MDBX_chk_none, false, true, false);
      EXPECT_EQ(MDBX_PROBLEM, rc) << "scope_push #" << at << " declined, verbosity " << verbosity;
      EXPECT_GE(last_total_problems, 1) << "scope_push #" << at << " declined, verbosity " << verbosity;
      EXPECT_EQ(entered, left) << "scope_push #" << at << " declined, verbosity " << verbosity;
      EXPECT_EQ(0, mismatch_issues) << "no phantom mismatches for the declined scope_push #" << at << ", verbosity "
                                    << verbosity;
    }
  }
}

/* Declining the GC-scope: without the GC data the used/GC pages mismatch
 * checks must be skipped (no phantom issues), the refusal must be accounted. */
TEST(issue_gh0314, chk_declined_gc_scope_has_no_phantom_problems) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = 0;
    MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
    const int rc = mdbx_dbi_open(txn, "table", MDBX_CREATE, &dbi);
    if (rc != MDBX_SUCCESS || mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      FAIL() << "could not fill the database";
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  MDBX_chk_context_t ctx;
  for (const MDBX_chk_severity_t verbosity : {MDBX_chk_result, MDBX_chk_extra}) {
    const int rc = run_chk_full(env, verbosity, &ctx, 0, 1, MDBX_chk_gc, false, true, false);
    EXPECT_EQ(MDBX_PROBLEM, rc) << "verbosity " << verbosity;
    EXPECT_GE(ctx.result.problems_gc, 1) << "verbosity " << verbosity;
    EXPECT_GE(last_total_problems, 1) << "verbosity " << verbosity;
    EXPECT_EQ(0, mismatch_issues) << "verbosity " << verbosity;
    EXPECT_EQ(entered, left) << "verbosity " << verbosity;
  }
}

/* Declining the B-Trees traversal scope: the GC and KV stages must be skipped
 * (the tree_problems seeding), with the refusal accounted. */
TEST(issue_gh0314, chk_declined_tree_scope_skips_gc_and_kv) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));

  MDBX_chk_context_t ctx;
  const int rc = run_chk_full(env, MDBX_chk_extra, &ctx, 0, 1, MDBX_chk_tree, false, true, false);
  EXPECT_EQ(MDBX_PROBLEM, rc);
  EXPECT_GE(ctx.result.tree_problems, 1);
  EXPECT_GE(ctx.result.gc_tree_problems, 1) << "the GC stage must be seeded from the declined B-Trees";
  EXPECT_GE(ctx.result.kv_tree_problems, 1) << "the KV stage must be seeded from the declined B-Trees";
  EXPECT_EQ(entered, left);
}

/* Declining the meta-scope: without the meta data none of the checks could be
 * performed, so the check must stop with MDBX_PROBLEM (no cascade of issues
 * from the sections operating on absent data). */
TEST(issue_gh0314, chk_declined_meta_scope_stops_the_check) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));

  MDBX_chk_context_t ctx;
  const int rc = run_chk_full(env, MDBX_chk_extra, &ctx, 0, 1, MDBX_chk_meta, false, true, false);
  EXPECT_EQ(MDBX_PROBLEM, rc);
  EXPECT_GE(ctx.result.problems_meta, 1);
  EXPECT_EQ(entered, left);
}
