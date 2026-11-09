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
} // namespace

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
