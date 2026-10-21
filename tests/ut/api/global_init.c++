#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST(ut_global_init, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = NULL;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close_ex(env, false));
}
