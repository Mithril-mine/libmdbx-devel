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
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/* The cursor-facing read contracts that the verification campaign
 * (issue #485) confirmed green, complementary to get_cached (stairway /
 * multithreaded churn / error paths), cache-get (entry helpers, basic
 * HIT/miss) and the C++ estimate suite (exact counts):
 *
 *  - mdbx_cache_get(): the INV-CACHE1 ordering-filter contract
 *    (trunk_txnid > last_confirmed_txnid is MDBX_INVALID + CACHE_ERROR) and
 *    the deterministic single-thread status machine: REFRESHED on the first
 *    fill, HIT on the repeat, NOTFOUND mirrored through REFRESHED then HIT,
 *    the stale-entry rewrite after a writer commit, and BEHIND for a
 *    long-lived reader whose snapshot predates the entry;
 *  - mdbx_estimate_range(): FIRST..LAST is the exact item count (and zero
 *    for an empty table), the single-key and MDBX_EPSILON forms are exact,
 *    a DUPSORT single-key range yields the duplicate count, and a reversed
 *    range yields a negative distance.
 *
 * known contract gaps V-043/V-058 (the past-end dual encoding makes
 * estimate_distance = -1 / cursor_distance = -2 / estimate_range = 0 for
 * one logical range, and the items*diff multiplication wraps past
 * PTRDIFF_MAX on huge tables) - the ambiguous past-end inputs are
 * deliberately not asserted. */

namespace {

const char *kDb = "cursor-contracts.mdbx";

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

struct txn_ptr {
  MDBX_txn *txn = nullptr;
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

MDBX_val key_of(char *buf, unsigned i) {
  std::snprintf(buf, 16, "key-%04u", i);
  return MDBX_val{buf, std::strlen(buf)};
}

void put_str(MDBX_txn *txn, MDBX_dbi dbi, const char *k, const char *v) {
  MDBX_val key = {const_cast<char *>(k), std::strlen(k)}, data = {const_cast<char *>(v), std::strlen(v)};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
}

} // namespace

TEST(cursor_contracts, cache_status_machine) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 4));
  /* NOSTICKYTHREADS: the long-lived reader below coexists with the
   * same-thread writer transaction of the stale-entry case */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS, 0644));

  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", MDBX_CREATE, &dbi));
    for (unsigned i = 0; i < 20; ++i) {
      char kb[16];
      const MDBX_val key = key_of(kb, i);
      MDBX_val value = {(void *)"value", 5};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, dbi, const_cast<MDBX_val *>(&key), &value, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }

  txn_ptr reader;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &reader));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(reader.txn, "t", MDBX_DB_DEFAULTS, &dbi));

  /* --- the INV-CACHE1 ordering filter --------------------------------- */
  {
    char kb[16];
    MDBX_val key = key_of(kb, 5), data;
    MDBX_cache_entry_t entry;
    mdbx_cache_init(&entry);
    entry.trunk_txnid = 5;
    entry.last_confirmed_txnid = 3; /* newer than the trunk: impossible */
    const MDBX_cache_result_t r = mdbx_cache_get(reader.txn, dbi, &key, &data, &entry);
    EXPECT_EQ(MDBX_INVALID, r.errcode);
    EXPECT_EQ(MDBX_CACHE_ERROR, r.status);
  }

  /* --- the deterministic status machine -------------------------------- */
  char kb[16];
  const MDBX_val probe = key_of(kb, 5);
  MDBX_cache_entry_t entry;
  mdbx_cache_init(&entry);
  MDBX_val data;
  {
    const MDBX_cache_result_t first = mdbx_cache_get(reader.txn, dbi, &probe, &data, &entry);
    EXPECT_EQ(MDBX_SUCCESS, first.errcode);
    EXPECT_EQ(MDBX_CACHE_REFRESHED, first.status) << "the first fill publishes the entry";
    EXPECT_EQ(5u, data.iov_len);
    EXPECT_EQ(0, std::memcmp("value", data.iov_base, 5));
    const MDBX_cache_result_t again = mdbx_cache_get(reader.txn, dbi, &probe, &data, &entry);
    EXPECT_EQ(MDBX_SUCCESS, again.errcode);
    EXPECT_EQ(MDBX_CACHE_HIT, again.status) << "an unchanged snapshot hits";
  }
  {
    /* the absent key is cached as NOTFOUND: REFRESHED first, HIT after */
    MDBX_cache_entry_t absent;
    mdbx_cache_init(&absent);
    MDBX_val k = {(void *)"absent", 6};
    const MDBX_cache_result_t first = mdbx_cache_get(reader.txn, dbi, &k, &data, &absent);
    EXPECT_EQ(MDBX_NOTFOUND, first.errcode);
    EXPECT_EQ(MDBX_CACHE_REFRESHED, first.status);
    const MDBX_cache_result_t again = mdbx_cache_get(reader.txn, dbi, &k, &data, &absent);
    EXPECT_EQ(MDBX_NOTFOUND, again.errcode);
    EXPECT_EQ(MDBX_CACHE_HIT, again.status);
  }

  /* --- the stale-entry rewrite after a writer commit ------------------- */
  {
    txn_ptr writer;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &writer));
    MDBX_dbi wdbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(writer.txn, "t", MDBX_DB_DEFAULTS, &wdbi));
    put_str(writer.txn, wdbi, "key-0005", "newval");
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(writer.release()));
  }
  {
    txn_ptr fresh;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &fresh));
    MDBX_dbi rdbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(fresh.txn, "t", MDBX_DB_DEFAULTS, &rdbi));
    const MDBX_cache_result_t r = mdbx_cache_get(fresh.txn, rdbi, &probe, &data, &entry);
    EXPECT_EQ(MDBX_SUCCESS, r.errcode);
    EXPECT_EQ(MDBX_CACHE_REFRESHED, r.status) << "a stale entry is rewritten on the newer snapshot";
    EXPECT_EQ(6u, data.iov_len);
    EXPECT_EQ(0, std::memcmp("newval", data.iov_base, 6));
  }

  /* --- BEHIND: an old snapshot cannot use a newer entry ---------------- */
  /* reader still holds the pre-commit snapshot; entry now reflects the
   * post-commit trunk, so cache_get must bypass the cache (BEHIND) and
   * still deliver the snapshot-correct old value */
  {
    const MDBX_cache_result_t r = mdbx_cache_get(reader.txn, dbi, &probe, &data, &entry);
    EXPECT_EQ(MDBX_SUCCESS, r.errcode);
    EXPECT_EQ(MDBX_CACHE_BEHIND, r.status);
    EXPECT_EQ(5u, data.iov_len);
    EXPECT_EQ(0, std::memcmp("value", data.iov_base, 5));
  }
}

TEST(cursor_contracts, estimate_first_last_single_key) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));
  constexpr unsigned kItems = 100, kDups = 3;

  {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi plain, dups, empty;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "plain", MDBX_CREATE, &plain));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "dups", MDBX_DUPSORT | MDBX_CREATE, &dups));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "empty", MDBX_CREATE, &empty));
    for (unsigned i = 0; i < kItems; ++i) {
      char kb[16];
      const MDBX_val key = key_of(kb, i);
      MDBX_val value = {(void *)"v", 1};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, plain, const_cast<MDBX_val *>(&key), &value, MDBX_UPSERT));
    }
    MDBX_val dkey = {(void *)"dup-key", 7};
    for (unsigned i = 0; i < kDups; ++i) {
      char vb[8];
      std::snprintf(vb, sizeof vb, "v%u", i);
      MDBX_val value = {vb, std::strlen(vb)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, dups, &dkey, &value, MDBX_APPENDDUP));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }

  txn_ptr txn;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi plain, dups, empty;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "plain", MDBX_DB_DEFAULTS, &plain));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "dups", MDBX_DUPSORT, &dups));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "empty", MDBX_DB_DEFAULTS, &empty));
  ptrdiff_t distance = -777;
  const MDBX_val *const kFirst = nullptr; /* explicit FIRST */
  const MDBX_val *const kLast = nullptr;  /* explicit LAST */
  const MDBX_val *const kEpsilon = reinterpret_cast<const MDBX_val *>((intptr_t)-1);

  /* FIRST..LAST is the exact item count; an empty table yields zero */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, plain, kFirst, nullptr, kLast, nullptr, &distance));
  EXPECT_EQ((ptrdiff_t)kItems, distance);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, empty, kFirst, nullptr, kLast, nullptr, &distance));
  EXPECT_EQ(0, distance);

  /* the single-key and EPSILON forms are exact */
  char kb[16], kb2[16];
  const MDBX_val mid = key_of(kb, 40), mid2 = key_of(kb2, 40);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, plain, &mid, nullptr, &mid2, nullptr, &distance));
  EXPECT_EQ(1, distance);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, plain, &mid, nullptr, kEpsilon, nullptr, &distance));
  EXPECT_EQ(1, distance) << "key..+epsilon";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, plain, kEpsilon, nullptr, &mid, nullptr, &distance));
  EXPECT_EQ(1, distance) << "-epsilon..key";

  /* a DUPSORT single-key range yields the duplicate count */
  MDBX_val dkey = {(void *)"dup-key", 7}, dkey2 = {(void *)"dup-key", 7};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, dups, &dkey, nullptr, &dkey2, nullptr, &distance));
  EXPECT_EQ((ptrdiff_t)kDups, distance);

  /* a reversed range yields a negative distance */
  const MDBX_val lo = key_of(kb, 10), hi = key_of(kb2, 30);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn.txn, plain, &hi, nullptr, &lo, nullptr, &distance));
  EXPECT_GT(0, distance);

  /* known contract gaps V-043/V-058 (issue #485): the past-end dual
   * encoding and the items*diff wrap - the ambiguous past-end inputs are
   * deliberately not asserted here. */
}

TEST(cursor_contracts, past_end_dual_encoding_characterized) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS, 0644));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "dual", MDBX_CREATE, &dbi));
    for (unsigned i = 1; i <= 9; ++i) {
      char kb[8];
      const int klen = std::snprintf(kb, sizeof kb, "k%u", i);
      MDBX_val key = {kb, size_t(klen)}, value = {kb, size_t(klen)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "dual", MDBX_DB_DEFAULTS, &dbi));
  MDBX_cursor *a = nullptr, *b = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &a));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &b));

  /* cursor A: land on the last row, then seek past the end - the seek
   * fast path fires because A already sits on the rightmost leaf */
  MDBX_val k = {(void *)"k9", 2}, v, big = {(void *)"zzz", 3};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(a, &k, &v, MDBX_SET));
  const int past_rc = mdbx_cursor_get(a, &big, &v, MDBX_SET_LOWERBOUND);
  EXPECT_TRUE(past_rc == MDBX_NOTFOUND || past_rc == MDBX_RESULT_TRUE);

  /* the zone introspection pins the dual encoding itself: a pointed,
   * eof-marked cursor with ki == nkeys on the fast path
   * (the sibling-failure path instead lands on ki == nkeys - 1:
   * known contract gap V-029, the two encodings are not unified) */
  unsigned zone = 0;
  int top = -1;
  unsigned ki_top = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_tst_cursor_state(a, &zone, &top, &ki_top));
  EXPECT_GE(top, 0) << "the past-end fast-path cursor stays pointed";
  EXPECT_NE(0u, zone & MDBX_TST_Z_EOF_SOFT);
  EXPECT_NE(0u, zone & MDBX_TST_Z_EOF_HARD);
  EXPECT_EQ(0u, zone & MDBX_TST_Z_HOLLOW);
  EXPECT_EQ(9u, ki_top) << "ki == nkeys on the fast-path encoding";

  /* cursor B: the last row */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(b, &k, &v, MDBX_LAST));

  /* One logical range (begin past-end, end LAST), true signed rank -1.
   * The observed answers across the public rank/estimate APIs - pinned
   * as the CURRENT behavior, to be updated together with the fix:
   * known contract gaps V-043/V-037 (R1-uniformity at the estimate layer). */
  ptrdiff_t est = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_distance(a, b, &est));
  EXPECT_EQ(-1, est) << "literal ki delta - correct by coincidence (V-043)";

  intptr_t dist0 = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_distance(a, b, &dist0, 0));
  /* the eof_hard rank adjust stacks on the ki==nkeys encoding: -2 */
  EXPECT_EQ(intptr_t(-2), dist0) << "known contract gap V-043: the wrap-around answer";

  intptr_t dist1 = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_distance(a, b, &dist1, 1));
  EXPECT_TRUE(dist1 == -1 || dist1 == 1) << "deepness=1 skips the adjust (V-043)";

  ptrdiff_t rng = 0;
  MDBX_val beginkey = {(void *)"zzz", 3};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_range(txn, dbi, &beginkey, nullptr, nullptr, nullptr, &rng));
  EXPECT_EQ(0, rng) << "a fresh begin cursor takes the sibling-failure encoding (V-029/V-043)";

  mdbx_cursor_close(a);
  mdbx_cursor_close(b);
  mdbx_txn_abort(txn);
}
