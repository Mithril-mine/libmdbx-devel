#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace {

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

/* The file size via the C stdio, avoiding any platform-specific stat APIs. */
long long file_size(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return -1;
  std::fseek(f, 0, SEEK_END);
  const long long size = std::ftell(f);
  std::fclose(f);
  return size;
}

} // namespace

/* Regression for https://github.com/Mithril-mine/libmdbx-devel/issues/450:
 * copying an environment onto its own (still live) data file, or onto its
 * lock file, destroys the live database: osal_fsetsize(fd, 0) truncates the
 * mapped file, and with the legacy per-process F_SETLK locking no lock
 * conflict detects it (POSIX F_SETLK locks never conflict within one
 * process, and no flock is held on a data file in the operational mode).
 * The copy must be rejected with MDBX_EINVAL by the file-identity check
 * before any lock or destructive step. */
TEST(issue_gh0450, copy_onto_own_live_dxb_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
#ifdef _WIN32
  /* the legacy multi-open is a POSIX-only mode */
#else
  /* Exercise the legacy per-process locking: it cannot detect same-process
   * conflicts, so without the fix the copy destroys the database. The flag
   * is consumed at the library initialization, i.e. must be set before the
   * first mdbx call (the env_files_guard below). */
  ASSERT_EQ(0, setenv("MDBX_DBG_LEGACY_MULTIOPEN", "1", 1));
#endif

  const char *const kDb = "issue450.mdbx";
  const env_files_guard files({kDb});

  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  char value[256];
  for (int i = 0; i < 100; ++i) {
    char key[16];
    std::snprintf(key, sizeof key, "key%03d", i);
    std::memset(value, 'a' + (i % 26), sizeof value);
    MDBX_val k = {key, std::strlen(key)}, v = {value, sizeof value};
    ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  const long long size_before = file_size(kDb);
  ASSERT_GT(size_before, 0);

  /* every kind of the copy onto the env's own data file must be rejected */
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_copy(env.get(), kDb, MDBX_CP_OVERWRITE));
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_copy(env.get(), kDb, MDBX_CP_DEFAULTS));
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_copy(env.get(), kDb, MDBX_CP_COMPACT));

  /* and onto the lock file too (NOSUBDIR: the "<db>-lck" file) */
  const std::string lck_path = std::string(kDb) + "-lck";
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_copy(env.get(), lck_path.c_str(), MDBX_CP_OVERWRITE));

  /* the live database must be intact */
  EXPECT_EQ(size_before, file_size(kDb));
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  int count = 0;
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val k, v;
  int rc;
  for (rc = mdbx_cursor_get(cursor, &k, &v, MDBX_FIRST); rc == MDBX_SUCCESS;
       rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT)) {
    ++count;
    EXPECT_EQ(v.iov_len, sizeof value);
  }
  EXPECT_EQ(MDBX_NOTFOUND, rc);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  EXPECT_EQ(100, count) << "the live database must keep all of its records";

  /* the environment must remain fully usable */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  {
    MDBX_val wk = {const_cast<void *>(static_cast<const void *>("after")), 5}, wv = {value, sizeof value};
    ASSERT_EQ(mdbx_put(txn, dbi, &wk, &wv, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}
