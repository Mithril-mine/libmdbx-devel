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

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/341
// mdbx_enumerate_tables() looked up each table's handle by comparing names with
// the process's table-handle registry (env->kvs[i].name, env->dbs_flags[],
// env->n_dbi) without dbi_lock, while other threads of the process changed it
// under that lock: a data race, which could make the comparator read a torn
// name (or freed memory with MDBX_ENABLE_DBI_LOCKFREE=0). The scan now holds
// dbi_lock; the internal audit, which already holds it, scans without taking it.
//
// enumerate_tables_while_handles_change: one thread enumerates the tables in
// read transactions while the main thread creates a table and aborts, and opens
// and closes the handle of another table, so the handle slots below a stable one
// change. The enumerating transaction first binds the stable "high" handle, so
// the scan compares the changing slots. Every enumeration must succeed and find
// the committed table "kept" under its handle. Without the fix the comparator
// reads a torn name and the test usually crashes (null pointer in memcmp);
// ThreadSanitizer also reports the race in mdbx_enumerate_tables().
//
// audit_with_named_table: with MDBX_CHECKING=3 builds and MDBX_DBG_AUDIT, every
// commit audits the database through mdbx_enumerate_tables() while holding
// dbi_lock; the scan must not take it again. Skips in other builds.

namespace {

const char *const kDb = "issue341.mdbx";

/* Deletes the database before the test and on every exit path. */
struct files_guard {
  files_guard() { mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE); }
  ~files_guard() { mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE); }
};

struct env_guard {
  MDBX_env *env = nullptr;
  ~env_guard() {
    if (env)
      mdbx_env_close(env);
  }
};

struct seen_ctx {
  MDBX_dbi kept;
  int found, wrong_handle;
};

int find_kept(void *ctx, const MDBX_txn *, const MDBX_val *name, MDBX_db_flags_t, const struct MDBX_stat *,
              MDBX_dbi dbi) noexcept {
  seen_ctx *const seen = static_cast<seen_ctx *>(ctx);
  if (name->iov_len == 4 && memcmp(name->iov_base, "kept", 4) == 0) {
    ++seen->found;
    if (dbi != seen->kept)
      ++seen->wrong_handle;
  }
  return MDBX_SUCCESS;
}

int create_tables(MDBX_env *env, const char *const *names, size_t count, MDBX_dbi *dbis) {
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    return rc;
  for (size_t i = 0; rc == MDBX_SUCCESS && i < count; ++i)
    rc = mdbx_dbi_open(txn, names[i], MDBX_CREATE, &dbis[i]);
  if (rc == MDBX_SUCCESS)
    return mdbx_txn_commit(txn);
  mdbx_txn_abort(txn);
  return rc;
}

/* The MDBX_CHECKING value from the build options, -1 if not found. */
int build_checking() {
  const std::string options = mdbx_build.options ? mdbx_build.options : "";
  const auto at = options.find("CHECKING=");
  return (at == std::string::npos) ? -1 : atoi(options.c_str() + at + 9);
}

} // namespace

TEST(issue_gh0341, enumerate_tables_while_handles_change) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files;
  env_guard db;
  ASSERT_EQ(mdbx_env_create(&db.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(db.env, 8), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(db.env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  /* "kept" and "high" stay open; the slot of "other", below "high", is reused by "temp" and "other" in the loop */
  const char *const names[] = {"kept", "other", "spare", "high"};
  MDBX_dbi dbis[4] = {0, 0, 0, 0};
  int rc = create_tables(db.env, names, 4, dbis);
  ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
  ASSERT_EQ(mdbx_dbi_close(db.env, dbis[1]), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_close(db.env, dbis[2]), MDBX_SUCCESS);
  const MDBX_dbi kept = dbis[0];

  std::atomic<bool> stop{false};
  std::atomic<int> enum_rc{MDBX_SUCCESS}, missed{0}, wrong_handle{0};
  std::atomic<long> enumerations{0};
  std::thread enumerator([&] {
    do {
      MDBX_txn *rtxn = nullptr;
      int err = mdbx_txn_begin(db.env, nullptr, MDBX_TXN_RDONLY, &rtxn);
      seen_ctx seen = {kept, 0, 0};
      if (err == MDBX_SUCCESS) {
        MDBX_dbi high = 0; /* binds the stable handle above the changing slots to this transaction */
        err = mdbx_dbi_open(rtxn, "high", MDBX_DB_DEFAULTS, &high);
        if (err == MDBX_SUCCESS)
          err = mdbx_enumerate_tables(rtxn, find_kept, &seen);
        mdbx_txn_abort(rtxn);
      }
      if (err != MDBX_SUCCESS) {
        enum_rc.store(err);
        break;
      }
      if (seen.found != 1)
        missed.fetch_add(1);
      wrong_handle.fetch_add(seen.wrong_handle);
      enumerations.fetch_add(1);
    } while (!stop.load());
  });

  MDBX_txn *txn = nullptr;
  int changes = 0, change_rc = MDBX_SUCCESS;
  const auto start = std::chrono::steady_clock::now();
  while (change_rc == MDBX_SUCCESS && enum_rc.load() == MDBX_SUCCESS) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(10) || (elapsed > std::chrono::seconds(1) && enumerations.load() >= 100))
      break;
    /* a table created and then aborted: its handle's name is cleared */
    change_rc = mdbx_txn_begin(db.env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (change_rc == MDBX_SUCCESS) {
      MDBX_dbi temp = 0;
      change_rc = mdbx_dbi_open(txn, "temp", MDBX_CREATE, &temp);
      mdbx_txn_abort(txn);
    }
    /* a handle opened for the first time in a read transaction, then closed */
    if (change_rc == MDBX_SUCCESS)
      change_rc = mdbx_txn_begin(db.env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (change_rc == MDBX_SUCCESS) {
      MDBX_dbi other = 0;
      change_rc = mdbx_dbi_open(txn, "other", MDBX_DB_DEFAULTS, &other);
      mdbx_txn_abort(txn);
      if (change_rc == MDBX_SUCCESS)
        change_rc = mdbx_dbi_close(db.env, other);
    }
    ++changes;
  }
  stop.store(true);
  enumerator.join();

  EXPECT_EQ(change_rc, MDBX_SUCCESS) << "after " << changes << " changes: " << mdbx_strerror(change_rc);
  EXPECT_EQ(enum_rc.load(), MDBX_SUCCESS) << mdbx_strerror(enum_rc.load());
  EXPECT_GE(enumerations.load(), 100) << "the enumerating thread hardly ran";
  EXPECT_EQ(missed.load(), 0) << "enumerations that did not see the committed table";
  EXPECT_EQ(wrong_handle.load(), 0) << "enumerations that reported another handle for the committed table";
}

TEST(issue_gh0341, audit_with_named_table) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  if (build_checking() < 3)
    GTEST_SKIP() << "the audit needs a MDBX_CHECKING=3 build (" << mdbx_build.options << ")";
  files_guard files;
  env_guard db;
  ASSERT_EQ(mdbx_env_create(&db.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(db.env, 8), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(db.env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  const int previous = mdbx_setup_debug(
      MDBX_LOG_DONTCHANGE, static_cast<MDBX_debug_flags_t>(MDBX_DBG_ASSERT | MDBX_DBG_AUDIT), MDBX_LOGGER_DONTCHANGE);
  ASSERT_GE(previous, 0);
  const char *const names[] = {"named"};
  MDBX_dbi dbi = 0;
  const int rc = create_tables(db.env, names, 1, &dbi); /* the commit audits the database */
  /* the previous debug flags are in bits 0-15 */
  mdbx_setup_debug(MDBX_LOG_DONTCHANGE, static_cast<MDBX_debug_flags_t>(previous & 0xffff), MDBX_LOGGER_DONTCHANGE);
  EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
}
