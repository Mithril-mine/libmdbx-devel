#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/93
// mdbx_env_chk() pointed ctx.result.tables into its internal state and freed it
// before returning, leaving a dangling pointer; and the C++ env::chk() threw
// mdbx::internal_problem whenever the check found a problem, so the result of
// checking a damaged database was unreachable. result.tables is now reset to
// null after the check, and MDBX_PROBLEM with counted problems is reported
// through ctx, not as an exception.

TEST(issue_gh0093, tables_is_reset_after_chk) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue93.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
  {
    mdbx::txn_managed txn = env.start_write();
    txn.create_map("t");
    txn.commit();
  }

  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  /* The doc promised the tables pointer to stay valid after the call; the C
   * core actually freed it. After the fix it is reset to null, so a caller
   * cannot read freed memory. */
  MDBX_chk_context_t &chk_result = env.chk(ctx);
  (void)chk_result;
  EXPECT_EQ(nullptr, ctx.result.tables) << "tables must not dangle after the check";
}

TEST(issue_gh0093, chk_reports_problems_in_ctx_not_as_exception) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue93b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
  {
    mdbx::txn_managed txn = env.start_write();
    txn.create_map("t");
    txn.commit();
  }

  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  /* The callback reports a problem and returns MDBX_PROBLEM; the check must
   * return normally with the problem counted in ctx instead of throwing
   * mdbx::internal_problem. */
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof(cb));
  cb.stage_end = [](MDBX_chk_context_t *c, MDBX_chk_stage_t stage, int err) -> int {
    if (stage == MDBX_chk_finalize) {
      ::mdbx_env_chk_encount_problem(c);
      return MDBX_PROBLEM;
    }
    return err;
  };
  MDBX_chk_context_t &chk_problem = env.chk(ctx, &cb);
  (void)chk_problem;
  EXPECT_GT(ctx.result.total_problems, (size_t)0) << "the reported problem must be counted in ctx";
}