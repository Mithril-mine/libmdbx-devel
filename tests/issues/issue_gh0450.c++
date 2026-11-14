/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/450:
 * copying an environment onto its own (still live) data file, or onto its
 * lock file, destroys the live database: the copy truncates the mapped file
 * and, in the self-copy case, then fails and unlinks it. With the legacy
 * per-process F_SETLK locking no lock conflict detects this (POSIX F_SETLK
 * locks never conflict within one process, and no flock is held on a data
 * file in the operational mode). The copy must be rejected with MDBX_EINVAL
 * by the file-identity check before any lock or destructive step.
 *
 * Runs with MDBX_DBG_LEGACY_MULTIOPEN=1 to exercise the per-process locking
 * (POSIX only; the flag is consumed at the library initialization, so it is
 * set before the first mdbx call). */
#include "mdbx.h++"
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

static long long file_size(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return -1;
  std::fseek(f, 0, SEEK_END);
  const long long size = std::ftell(f);
  std::fclose(f);
  return size;
}

int main() {
#ifndef _WIN32
  if (setenv("MDBX_DBG_LEGACY_MULTIOPEN", "1", 1) != 0)
    return EXIT_FAILURE;
#endif

  const char *const db = "issue450.mdbx";
  char lck_path[64];
  std::snprintf(lck_path, sizeof lck_path, "%s-lck", db);
  mdbx::env::remove(db); /* removes both the data file and the lock file */

  int rc;
  MDBX_env *env = nullptr;
  rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return EXIT_FAILURE;
  rc = mdbx_env_open(env, db, MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: open rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  char value[256];
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  for (int i = 0; rc == MDBX_SUCCESS && i < 100; ++i) {
    char key[16];
    std::snprintf(key, sizeof key, "key%03d", i);
    std::memset(value, 'a' + (i % 26), sizeof value);
    MDBX_val k = {key, std::strlen(key)}, v = {value, sizeof value};
    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: fill rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  const long long size_before = file_size(db);
  if (size_before <= 0) {
    std::fprintf(stderr, "FAIL: cannot stat the database\n");
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* every kind of the copy onto the env's own files must be rejected */
  rc = mdbx_env_copy(env, db, MDBX_CP_OVERWRITE);
  if (rc != MDBX_EINVAL) {
    std::fprintf(stderr, "FAIL: OVERWRITE self-copy rc %d, expected MDBX_EINVAL\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }
  rc = mdbx_env_copy(env, db, MDBX_CP_DEFAULTS);
  if (rc != MDBX_EINVAL) {
    std::fprintf(stderr, "FAIL: default self-copy rc %d, expected MDBX_EINVAL\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }
  rc = mdbx_env_copy(env, db, MDBX_CP_COMPACT);
  if (rc != MDBX_EINVAL) {
    std::fprintf(stderr, "FAIL: compact self-copy rc %d, expected MDBX_EINVAL\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }
  rc = mdbx_env_copy(env, lck_path, MDBX_CP_OVERWRITE);
  if (rc != MDBX_EINVAL) {
    std::fprintf(stderr, "FAIL: copy onto the lock file rc %d, expected MDBX_EINVAL\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* the live database must be intact */
  if (file_size(db) != size_before) {
    std::fprintf(stderr, "FAIL: the database file size changed\n");
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS) {
    MDBX_cursor *cursor = nullptr;
    rc = mdbx_cursor_open(txn, dbi, &cursor);
    int count = 0;
    /* zero-initialized: MSVC's C4701 cannot prove that the ternary-filled
     * for-loop below always fills them before the use */
    MDBX_val k = {nullptr, 0}, v = {nullptr, 0};
    for (rc = rc == MDBX_SUCCESS ? mdbx_cursor_get(cursor, &k, &v, MDBX_FIRST) : rc; rc == MDBX_SUCCESS;
         rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT)) {
      if (v.iov_len != sizeof value) {
        std::fprintf(stderr, "FAIL: a record value is truncated\n");
        rc = MDBX_CORRUPTED;
        break;
      }
      ++count;
    }
    if (rc == MDBX_NOTFOUND)
      rc = MDBX_SUCCESS;
    if (cursor)
      mdbx_cursor_close(cursor);
    if (rc == MDBX_SUCCESS && count != 100) {
      std::fprintf(stderr, "FAIL: %d records instead of 100\n", count);
      rc = MDBX_CORRUPTED;
    }
    mdbx_txn_abort(txn);
  }
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: the live database is not intact, rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* the environment must remain fully usable */
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS) {
    MDBX_val k = {const_cast<void *>(static_cast<const void *>("after")), 5}, v = {value, sizeof value};
    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_txn_commit(txn);
    else
      mdbx_txn_abort(txn);
  }
  mdbx_env_close(env);
  mdbx::env::remove(db); /* removes both the data file and the lock file */
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: the environment is not usable, rc %d\n", rc);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
