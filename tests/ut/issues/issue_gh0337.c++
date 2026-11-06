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
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/337
// MDBX_CP_RENEW_TXN was never used: a copy with MDBX_CP_THROTTLE_MVCC whose parked
// read transaction was ousted by a writer before any data was copied failed with
// MDBX_OUSTED, although the flag asks for a restart (and mdbx_env_copy() always
// passes it). Now the first unpark of the copy restarts an ousted transaction.
//
// A writer thread rewrites a database whose size is fixed, so it has to oust the
// parked copy to reuse its pages. The copy_parked_stall_ms FAULT site makes the
// copy stall while parked, before its first unpark (as on a slow destination),
// until the writer has finished and disarms the site. The copy must succeed and
// hold every record, and the transaction must have moved to a newer snapshot.
// Requires MDBX_PROBES=ON and MDBX_PROBES=1.

namespace {

const char *const kSrc = "issue337-src.mdbx";
const char *const kCopy = "issue337-copy.mdbx";
constexpr int kRecords = 500;
constexpr size_t kValueLen = 2000;
constexpr intptr_t kSize = intptr_t(4) << 20;

/* Deletes both databases before the test and on every exit path. */
struct files_guard {
  files_guard() { remove_all(); }
  ~files_guard() {
    char reply[256];
    mdbx_probe_ctl("fault copy_parked_stall_ms none", reply, sizeof reply);
    remove_all();
  }
  static void remove_all() {
    mdbx_env_delete(kSrc, MDBX_ENV_JUST_DELETE);
    mdbx_env_delete(kCopy, MDBX_ENV_JUST_DELETE);
  }
};

struct env_guard {
  MDBX_env *env = nullptr;
  ~env_guard() {
    if (env)
      mdbx_env_close(env);
  }
};

/* Rewrites every record in each transaction; returns the first error other than MDBX_MAP_FULL. */
int rewrite_all(MDBX_env *env, MDBX_dbi dbi, unsigned round) {
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    return rc;
  char value[kValueLen];
  memset(value, 'a' + round % 26, sizeof(value));
  for (int i = 0; rc == MDBX_SUCCESS && i < kRecords; ++i) {
    char key[16];
    snprintf(key, sizeof key, "k%06d", i);
    MDBX_val k = {key, strlen(key)}, v = {value, sizeof(value)};
    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
  }
  if (rc == MDBX_SUCCESS)
    return mdbx_txn_commit(txn);
  mdbx_txn_abort(txn);
  return (rc == MDBX_MAP_FULL) ? MDBX_SUCCESS : rc;
}

/* Returns the hits of the FAULT site, or 0 when it was never reached. */
size_t site_hits() {
  char reply[512];
  if (mdbx_probe_ctl("query copy_parked_stall_ms", reply, sizeof reply) != 0)
    return 0;
  const char *const site = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (site && sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

size_t count_records(const char *path) {
  env_guard copy;
  if (mdbx_env_create(&copy.env) != MDBX_SUCCESS ||
      mdbx_env_open(copy.env, path, MDBX_NOSUBDIR | MDBX_RDONLY, 0644) != MDBX_SUCCESS)
    return 0;
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(copy.env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return 0;
  MDBX_dbi dbi = 0;
  MDBX_stat st;
  size_t entries = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) == MDBX_SUCCESS &&
      mdbx_dbi_stat(txn, dbi, &st, sizeof(st)) == MDBX_SUCCESS)
    entries = st.ms_entries;
  mdbx_txn_abort(txn);
  return entries;
}

} // namespace

TEST(issue_gh0337, renew_txn_restarts_an_ousted_parked_copy) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files;
  env_guard src;
  ASSERT_EQ(mdbx_env_create(&src.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(src.env, kSize, kSize, kSize, -1, -1, 4096), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(src.env, kSrc, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(src.env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  int rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
  ASSERT_EQ(rewrite_all(src.env, dbi, 0), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(src.env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  const uint64_t snapshot = mdbx_txn_id(txn);

  /* The writer starts once the copy stalls parked, makes enough rounds to oust it, and only then ends the stall: a
   * writer still running after the stall could oust the copy again at its later (throttling) unparks, which do not
   * restart. */
  std::atomic<bool> stop{false};
  std::atomic<int> writer_rc{MDBX_SUCCESS};
  std::atomic<unsigned> rounds{0};
  std::thread writer([&] {
    while (!stop.load() && site_hits() == 0)
      std::this_thread::yield();
    for (unsigned round = 1; !stop.load() && round <= 10; ++round) {
      const int err = rewrite_all(src.env, dbi, round);
      if (err != MDBX_SUCCESS) {
        writer_rc.store(err);
        break;
      }
      rounds.store(round);
    }
    char ctl_reply[256]; /* the writer is done: the copy goes on */
    mdbx_probe_ctl("fault copy_parked_stall_ms none", ctl_reply, sizeof ctl_reply);
  });

  char reply[512];
  rc = mdbx_probe_ctl("reset copy_parked_stall_ms", reply, sizeof reply);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_probe_ctl("fault copy_parked_stall_ms 30000", reply, sizeof reply);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_copy2pathname(txn, kCopy, MDBX_CP_THROTTLE_MVCC | MDBX_CP_RENEW_TXN);
  else
    ADD_FAILURE() << "mdbx_probe_ctl(): " << reply;
  mdbx_probe_ctl("fault copy_parked_stall_ms none", reply, sizeof reply);
  const uint64_t restarted = mdbx_txn_id(txn);
  stop.store(true);
  writer.join();
  mdbx_txn_abort(txn);

  ASSERT_EQ(writer_rc.load(), MDBX_SUCCESS) << "the writer failed after " << rounds.load() << " rounds";
  ASSERT_EQ(rc, MDBX_SUCCESS) << "mdbx_txn_copy2pathname(MDBX_CP_THROTTLE_MVCC | MDBX_CP_RENEW_TXN): "
                              << mdbx_strerror(rc);
  EXPECT_NE(restarted, UINT64_MAX) << "the transaction was left ousted";
  EXPECT_GT(restarted, snapshot) << "the parked copy was not ousted and restarted (the writer did " << rounds.load()
                                 << " rounds)";
  EXPECT_EQ(count_records(kCopy), size_t(kRecords)) << "the copy does not hold every record";
}
