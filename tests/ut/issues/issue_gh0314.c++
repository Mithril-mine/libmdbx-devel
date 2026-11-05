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

int scope_push(MDBX_chk_context_t *, MDBX_chk_scope_t *, MDBX_chk_scope_t *, const char *, va_list) {
  if (!fail_stage_begin && ++calls == fail_at)
    return MDBX_EINTR;
  ++entered;
  return MDBX_SUCCESS;
}

void scope_pop(MDBX_chk_context_t *, MDBX_chk_scope_t *, MDBX_chk_scope_t *) { ++left; }

enum MDBX_chk_stage failed_stage;

int stage_begin(MDBX_chk_context_t *, enum MDBX_chk_stage stage) {
  if (!fail_stage_begin || ++calls != fail_at)
    return MDBX_SUCCESS;
  failed_stage = stage;
  return MDBX_EINTR;
}

/* Ignores the error of the stage whose `stage_begin` failed, which mdbx.h allows too. */
int stage_end(MDBX_chk_context_t *, enum MDBX_chk_stage stage, int err) {
  return (stage == failed_stage) ? MDBX_SUCCESS : err;
}

int run_chk(MDBX_env *env, MDBX_chk_severity_t verbosity, int at) {
  MDBX_chk_callbacks_t cb;
  MDBX_chk_context_t ctx;
  std::memset(&cb, 0, sizeof(cb));
  std::memset(&ctx, 0, sizeof(ctx));
  cb.scope_push = scope_push;
  cb.scope_pop = scope_pop;
  cb.stage_begin = stage_begin;
  if (fail_stage_begin)
    cb.stage_end = stage_end;
  calls = entered = left = 0;
  fail_at = at;
  failed_stage = MDBX_chk_none;
  return mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, verbosity, 0);
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
      /* a later call must not crash or corrupt the heap either; the check may go on or stop */
      for (int at = 1; at <= total; ++at) {
        run_chk(env, verbosity, at);
        EXPECT_EQ(entered, left) << what << " #" << at << " failed, verbosity " << verbosity;
      }
    }
  }
}
