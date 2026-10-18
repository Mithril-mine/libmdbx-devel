#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/114
// Three environment-API edge cases:
//  1. mdbx_env_warmup() accepted an environment that was created but not
//     opened and then dereferenced its unmapped database, crashing instead of
//     returning MDBX_EPERM like other calls that need the opened mapping.
//  3. default_rp_augment_limit() compared the gc_time_limit (monotonic units)
//     with a fixed-point-seconds timeframe, so any practical time limit
//     dropped the automatic rp_augment_limit to its minimum.
// Item 2 (Windows writethrough threshold round-trip) is platform-specific and
// is not exercised here.

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

TEST(issue_gh0114, warmup_requires_opened_env) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_NE(nullptr, env);
  /* The environment is created but not opened: the database is not mapped, so
   * warmup must be rejected, not crash. */
  EXPECT_EQ(MDBX_EPERM, mdbx_env_warmup(env, nullptr, MDBX_warmup_force, 0))
      << "warmup on a created-but-not-opened env must return MDBX_EPERM";
  mdbx_env_close(env);
}

TEST(issue_gh0114, gc_time_limit_keeps_rp_augment_limit) {
  const char *const testdb = "issue114.mdbx";
  remove_db(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  /* the current size must be large enough for one third of it to exceed the
   * rp_augment_limit minimum (2 * maxgc_large1page), otherwise both the fixed
   * and the broken scaling yield that minimum */
  ASSERT_EQ(MDBX_SUCCESS,
            mdbx_env_set_geometry(env, 16 * 1024 * 1024, 64 * 1024 * 1024, 64 * 1024 * 1024, -1, -1, 4096));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644)) << "the base environment must open";

  constexpr uint64_t gc_time_limit = 1u << 16 /* 1 second in 16.16 */;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_gc_time_limit, gc_time_limit));

  uint64_t rp_augment_limit = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_rp_augment_limit, &rp_augment_limit));
  /* The rp_augment_limit must not collapse to its minimum when a practical GC
   * time limit (1 second out of the 16-second scale) is set: full scale minus
   * 1/16 keeps ~94% of the headroom. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_gc_time_limit, UINT64_MAX));
  uint64_t no_limit = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_rp_augment_limit, &no_limit));
  EXPECT_GT(rp_augment_limit, no_limit * 3 / 4)
      << "a 1-second gc_time_limit must not drop rp_augment_limit to its minimum";

  mdbx_env_close(env);
  remove_db(testdb);
}