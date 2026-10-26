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

#include "../../../src/internals.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/203
//
// Write errors of the auxiliary (stub) writes used to be silently ignored:
//  - page_kill() scrubs a freed page with a 0xFF pattern when a page is
//    retired (reached on every retire in checking builds);
//  - the best-effort prefault-write pre-faults a freshly allocated page into
//    RAM (WRITEMAP mode only, and only for pages not already resident).
// Neither write carries database data, but a failure is the only indication of
// a failing media/filesystem on these paths. The fix routes the errors through
// the internal stub_write_errdecide() decision point (currently returning the
// error as-is) and surfaces them: the transaction breaks with the error code.
//
// The failures are injected deterministically via the probe-bus FAULT sites
// `stub_write_kill` and `stub_write_prefault`. Requires a build with
// MDBX_PROBES=ON and MDBX_PROBES=1 in the environment.
// NB: probe sites are registered lazily on first fire, so each site is
// "warmed up" by one successful operation before it can be armed.

namespace {

static const char *const kDbKill = "issue203-kill.mdbx";
static const char *const kDbPrefault = "issue203-prefault.mdbx";

/* Query a FAULT site and return its `hits` counter (times an armed fault was
 * injected), or -1 when the site record is absent / malformed.
 * Reply line format (see logging_and_debug.c "query"):
 *   site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file>:<line>
 */
static long probe_site_hits(const char *site_name) {
  char reply[512];
  char cmd[160];
  std::snprintf(cmd, sizeof cmd, "query %s", site_name);
  if (mdbx_probe_ctl(cmd, reply, sizeof reply) != 0)
    return -1;
  char *line = std::strstr(reply, "site ");
  if (!line)
    return -1;
  char name[128];
  unsigned kind = 0, armed = 0;
  unsigned long seen = 0, hits = 0, suppressed = 0, value = 0;
  if (std::sscanf(line, "site %127s %u %u %lu %lu %lu %lu", name, &kind, &armed, &seen, &hits, &suppressed, &value) < 5)
    return -1;
  return (long)hits;
}

/* Store a big multi-page (overflow) value, then a second big value (so the
 * first one's pages are no longer at the allocation top and cannot be simply
 * refunded), then shrink the first value — the released overflow pages span
 * >1 page, so they are not loosenable and go through page_kill() (scrubbed
 * with a stub). */
static void retire_overflow_page(MDBX_txn *txn, MDBX_dbi dbi, const char *key) {
  MDBX_val k{const_cast<char *>(key), std::strlen(key)};
  std::vector<char> big(20000, 'x'); /* > 4 pages of 4K -> multi-page overflow */
  MDBX_val bigval{big.data(), big.size()};
  char tiny[8] = {'t', 'i', 'n', 'y', '!', '!', '!', 0};
  MDBX_val tinyval{tiny, sizeof tiny};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &bigval, MDBX_UPSERT));
  /* push the allocation top past the big pages with a second big value */
  char kbuf[64];
  int kn = std::snprintf(kbuf, sizeof kbuf, "%s-anchor", key);
  MDBX_val ak{kbuf, size_t(kn)};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &ak, &bigval, MDBX_UPSERT));
  /* now shrinking the first big value retires its overflow pages via page_kill() */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &tinyval, MDBX_UPSERT));
}

TEST(issue_gh0203, page_kill_write_error_breaks_the_transaction) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  std::remove(kDbKill);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1));
  /* MDBX_PAGEPERTURB forces page_kill() to run on every page retire regardless
   * of the build's checking level, making the fault site deterministic. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbKill, MDBX_NOSUBDIR | MDBX_PAGEPERTURB, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));

  /* Warm-up: fire the `stub_write_kill` site once so the FAULT rule can be
   * armed (sites are registered lazily on first fire). */
  retire_overflow_page(txn, dbi, "warmup");

  char reply[512];
  ASSERT_EQ(0, mdbx_probe_ctl("fault stub_write_kill none", reply, sizeof reply));
  char cmd[128];
  std::snprintf(cmd, sizeof cmd, "fault stub_write_kill %d", (int)MDBX_EIO);
  ASSERT_EQ(0, mdbx_probe_ctl(cmd, reply, sizeof reply));

  /* The retire path must now surface the injected write error instead of
   * silently ignoring it: either the put or the commit fails with MDBX_EIO
   * and the transaction is broken. */
  int rc = MDBX_SUCCESS;
  {
    MDBX_val k{const_cast<char *>("victim"), 6};
    std::vector<char> big(20000, 'x');
    MDBX_val bigval{big.data(), big.size()};
    char tiny[8] = {'t', 'i', 'n', 'y', '!', '!', '!', 0};
    MDBX_val tinyval{tiny, sizeof tiny};
    rc = mdbx_put(txn, dbi, &k, &bigval, MDBX_UPSERT);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_put(txn, dbi, &k, &tinyval, MDBX_UPSERT);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_txn_commit(txn);
    else
      mdbx_txn_abort(txn);
  }
  ASSERT_EQ((int)MDBX_EIO, rc) << "the stub-write error must be surfaced, not swallowed";

  /* Disarm and verify the environment is still usable: a fresh transaction
   * must succeed (the error was transaction-level, not env-fatal). */
  ASSERT_EQ(0, mdbx_probe_ctl("fault stub_write_kill none", reply, sizeof reply));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  retire_overflow_page(txn, dbi, "after");
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  mdbx_env_close(env);
  std::remove(kDbKill);
}

TEST(issue_gh0203, prefault_write_error_breaks_the_transaction) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  std::remove(kDbPrefault);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 256 << 20, -1, -1, -1));
  /* MDBX_NORDAHEAD disables the readahead window, otherwise the prefault-write
   * is skipped for pages inside the readahead edge. MDBX_WRITEMAP is required
   * for the prefault path at all. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPrefault, MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_NORDAHEAD, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));

  /* The prefault-write is deactivated for a trivial database (fewer than 1234
   * pages with an empty GC b-tree), so grow the database well past that
   * threshold first; each 9000-byte value spans several pages, so the writes
   * exercise the scatter-gather stub path too. */
  {
    std::vector<char> val(9000, 'y'); /* multi-page: exercises the scatter-gather stub-write path */
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 0; i < 4000; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  /* Warm-up: allocate more pages in a fresh transaction, extending the file,
   * so the freshly extended (non-resident) pages fire the prefault site. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  {
    std::vector<char> val(9000, 'z');
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 4000; i < 5000; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  char reply[512];
  ASSERT_EQ(0, mdbx_probe_ctl("fault stub_write_prefault none", reply, sizeof reply));
  /* Zero the counters so `hits` reflects only the victim phase below. */
  ASSERT_EQ(0, mdbx_probe_ctl("reset stub_write_prefault", reply, sizeof reply));
  char cmd[128];
  std::snprintf(cmd, sizeof cmd, "fault stub_write_prefault %d", (int)MDBX_EIO);
  ASSERT_EQ(0, mdbx_probe_ctl(cmd, reply, sizeof reply));

  int rc = MDBX_SUCCESS;
  {
    MDBX_txn *victim = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &victim));
    std::vector<char> val(9000, 'w');
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 5000; rc == MDBX_SUCCESS && i < 6000; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      rc = mdbx_put(victim, dbi, &k, &v, MDBX_UPSERT);
    }
    if (rc == MDBX_SUCCESS)
      rc = mdbx_txn_commit(victim);
    else
      mdbx_txn_abort(victim);
  }
  if (rc == MDBX_SUCCESS) {
    /* Distinguish "the path is unreachable here" from "the error was swallowed
     * again": if the fault was actually injected (hits > 0) then returning
     * MDBX_SUCCESS is a regression and must fail, not skip. */
    const long hits = probe_site_hits("stub_write_prefault");
    ASSERT_EQ(0, mdbx_probe_ctl("fault stub_write_prefault none", reply, sizeof reply));
    mdbx_env_close(env);
    std::remove(kDbPrefault);
    ASSERT_EQ(0, hits) << "the prefault fault was injected but the error got swallowed (regression)";
    GTEST_SKIP() << "the prefault site was not reached (all pages in-core)";
  }
  ASSERT_EQ((int)MDBX_EIO, rc) << "the prefault write error must be surfaced, not swallowed";

  /* Disarm and verify the environment is still usable. */
  ASSERT_EQ(0, mdbx_probe_ctl("fault stub_write_prefault none", reply, sizeof reply));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  {
    char kbuf[32];
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", size_t(9));
    MDBX_val k{kbuf, size_t(kn)};
    char small[4] = {'o', 'k', '!', 0};
    MDBX_val v{small, sizeof small};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  mdbx_env_close(env);
  std::remove(kDbPrefault);
}

} // namespace
