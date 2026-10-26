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

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstring>

// Coverage for the MDBX_opt_shrink_retry_period option added for issue #170
// (rate-limiting of datafile-shrink attempts). The option itself is
// cross-platform (it is stored in the per-process env->options), while the
// throttling it controls only matters on Windows, where a shrink requires
// un-mapping the file — see issue_gh0170_throttle.c++ for the Windows-only
// behavior test and issue_gh0173.c++ for the deferred-shrink scenarios.

namespace {

const char *const kTestPath = "issue170-opt.mdbx";

void env_remove(const char *path) {
  try {
    mdbx::env::remove(mdbx::path(path));
  } catch (const mdbx::error &) {
    /* not required to exist beforehand */
  }
}

MDBX_env *create_env() {
  MDBX_env *env = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  return env;
}

uint64_t get_period(MDBX_env *env) {
  uint64_t value = 0;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_shrink_retry_period, &value));
  return value;
}

} // namespace

TEST(issue_gh0170, shrink_retry_period_defaults_to_5_seconds) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = create_env();
  ASSERT_NE(nullptr, env);
  /* the documented default: 5 seconds in 1/65536 units */
  EXPECT_EQ(5u * 65536u, get_period(env));
  mdbx_env_close(env);
}

TEST(issue_gh0170, shrink_retry_period_roundtrips) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = create_env();
  ASSERT_NE(nullptr, env);

  /* zero disables the throttling */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 0));
  EXPECT_EQ(0u, get_period(env));

  /* an arbitrary period round-trips exactly (stored in monotonic units) */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 65536 /* 1s */));
  EXPECT_EQ(65536u, get_period(env));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 42 * 65536 /* 42s */));
  EXPECT_EQ(42u * 65536u, get_period(env));

  /* UINT64_MAX resets to the default */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, UINT64_MAX));
  EXPECT_EQ(5u * 65536u, get_period(env));

  /* a value that does not fit 1/65536-of-second units is rejected */
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, UINT64_C(0x100000000)));
  EXPECT_EQ(5u * 65536u, get_period(env)) << "a rejected value must not change the option";

  mdbx_env_close(env);
}

TEST(issue_gh0170, shrink_retry_period_settable_after_open_but_not_readonly) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  env_remove(kTestPath);
  MDBX_env *env = create_env();
  ASSERT_NE(nullptr, env);
  /* a non-default value set before the open survives it */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 2 * 65536));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR, 0644));
  EXPECT_EQ(2u * 65536u, get_period(env));
  /* and can be changed while the environment is open */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 7 * 65536));
  EXPECT_EQ(7u * 65536u, get_period(env));
  mdbx_env_close(env);

  /* a read-only environment cannot change it */
  MDBX_env *ro_env = create_env();
  ASSERT_NE(nullptr, ro_env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(ro_env, kTestPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));
  EXPECT_EQ(MDBX_EACCESS, mdbx_env_set_option(ro_env, MDBX_opt_shrink_retry_period, 3 * 65536));
  /* reading is still allowed and reports the default */
  EXPECT_EQ(5u * 65536u, get_period(ro_env));
  mdbx_env_close(ro_env);

  env_remove(kTestPath);
}
