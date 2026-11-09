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

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/319 "API contract deviations (15 items)"

namespace {

const char *const kDb = "issue319.mdbx";
const int kMainDbi = 1;

void remove_db() {
  std::remove(kDb);
  std::remove((std::string(kDb) + MDBX_LOCK_SUFFIX).c_str());
}

struct db_remover {
  const char *path;
  explicit db_remover(const char *p) : path(p) { remove_db(); }
  ~db_remover() { remove_db(); }
};

MDBX_env *open_env(const char *path, MDBX_env_flags_t extra_flags) {
  MDBX_env *env = nullptr;
  EXPECT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  EXPECT_NE(env, nullptr);
  if (env) {
    EXPECT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
/* a 1 GiB upper bound is beyond the win32 limits */
#if MDBX_WORDBITS >= 64
    const size_t geometry_upper = 1024u << 20;
#else
    const size_t geometry_upper = 256u << 20;
#endif
    EXPECT_EQ(mdbx_env_set_geometry(env, -1, 1 << 20, geometry_upper, 1 << 20, -1, 4096), MDBX_SUCCESS);
    if (mdbx_env_open(env, path, MDBX_NOSUBDIR | extra_flags, 0644) != MDBX_SUCCESS) {
      mdbx_env_close(env);
      return nullptr;
    }
  }
  return env;
}

MDBX_val val_of(const char *s) {
  MDBX_val v = {(void *)s, strlen(s)};
  return v;
}

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *k, const char *v) {
  MDBX_val key = val_of(k), data = val_of(v);
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT), MDBX_SUCCESS);
}

} // namespace

//-----------------------------------------------------------------------------

/* Item 1: mdbx_cursor_renew(NULL, mc) must not dereference the null txn. */
TEST(issue_gh0319, pp1_renew_with_null_txn) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_cursor *cursor = mdbx_cursor_create(nullptr);
  ASSERT_NE(cursor, nullptr);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_bind(txn, cursor, kMainDbi), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_renew(nullptr, cursor), MDBX_EINVAL);
  mdbx_cursor_close(cursor);
  mdbx_env_close(env);
}

/* Item 2: mdbx_env_info_ex() on an environment that is not open must zero
   the fields it does not fill. */
TEST(issue_gh0319, pp2_info_of_unopened_env) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  MDBX_envinfo info;
  memset(&info, -1, sizeof(info)); /* fill with 0xFF bytes */
  EXPECT_EQ(mdbx_env_info_ex(env, nullptr, &info, sizeof(info)), MDBX_SUCCESS);
  EXPECT_EQ(info.mi_recent_txnid, 0u);
  EXPECT_EQ(info.mi_mode, 0u);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

/* Item 3: MDBX_NEXT_MULTIPLE/PREV_MULTIPLE require MDBX_DUPFIXED. */
TEST(issue_gh0319, pp3_multiple_requires_dupfixed) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  for (int i = 0; i < 5; ++i) {
    char v[16];
    snprintf(v, sizeof(v), "v%d", i);
    must_put(txn, dbi, "k", v);
  }
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("k"), data = val_of("v0");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_MULTIPLE), MDBX_INCOMPATIBLE);
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_PREV_MULTIPLE), MDBX_INCOMPATIBLE);
  /* the cursor must stay usable for the documented single-value ops */
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_DUP), MDBX_SUCCESS);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 4: mdbx_estimate_range() must honor begin_data/end_data, and
   mdbx_estimate_move() must treat MDBX_RESULT_TRUE as success. */
TEST(issue_gh0319, pp4_estimate_honors_data_bounds) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  char buf[32];
  for (int i = 0; i < 1000; ++i) {
    snprintf(buf, sizeof(buf), "%06dxxxxxxxxxxxxx", i);
    must_put(txn, dbi, "k", buf);
  }
  must_put(txn, dbi, "m", "1");

  ptrdiff_t distance = -7;
  MDBX_val begin_key = val_of("k"), end_key = val_of("k");
  MDBX_val begin_data = val_of("000000"), end_data = val_of("000900");
  EXPECT_EQ(mdbx_estimate_range(txn, dbi, &begin_key, &begin_data, &end_key, &end_data, &distance), MDBX_SUCCESS);
  EXPECT_GT(distance, 0) << "the data bounds were ignored";
  EXPECT_LT(distance, 1000);

  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("a"), data = val_of("1");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST), MDBX_SUCCESS);
  distance = -7;
  key = val_of("l");
  data = val_of("");
  EXPECT_EQ(mdbx_estimate_move(cursor, &key, &data, MDBX_SET_LOWERBOUND, &distance), MDBX_SUCCESS);
  EXPECT_EQ(distance, 1) << "MDBX_RESULT_TRUE of the move was treated as a failure";
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 5: mdbx_cursor_scan() with MDBX_NEXT_DUP over keys with a single value
   each must return MDBX_RESULT_FALSE, not MDBX_ENODATA. */
static int scan_predicate_never_matches(void *, MDBX_val *, MDBX_val *, void *) noexcept { return MDBX_RESULT_FALSE; }

TEST(issue_gh0319, pp5_scan_next_dup_single_values) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "a", "1");
  must_put(txn, dbi, "b", "1");
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_scan(cursor, scan_predicate_never_matches, nullptr, MDBX_FIRST, MDBX_NEXT_DUP, nullptr),
            MDBX_RESULT_FALSE);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 6: MDBX_opt_txn_dp_initial set before open must not be truncated. */
TEST(issue_gh0319, pp6_dp_initial_not_truncated) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_option(env, MDBX_opt_txn_dp_initial, 4096), MDBX_SUCCESS);
  uint64_t got = 0;
  ASSERT_EQ(mdbx_env_get_option(env, MDBX_opt_txn_dp_initial, &got), MDBX_SUCCESS);
  EXPECT_EQ(got, 4096u);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}
