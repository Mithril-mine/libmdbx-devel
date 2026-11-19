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
#include <vector>

/* The delete-after-tail DUPSORT contract, after the positioning rules
 * R1-R5 and the cursor_del pre/post conditions of the verification
 * campaign (docs/engineering/libmdbx-invariants.md section 5;
 * doc-additions-v5-b "cursor_del"): for every path that reaches the inner
 * tail of a dupset, the follow-up operations have one deterministic
 * outcome:
 *
 *  - consuming the duplicates with NEXT_DUP until MDBX_NOTFOUND leaves the
 *    eof-soft inner position on the tail row: GET_CURRENT still reads it
 *    (R3) and del removes exactly that row (the position is semantically
 *    valid);
 *  - afterwards the z_inner_tail_deleted state makes del a no-op that
 *    still reports MDBX_SUCCESS, and repeating it changes nothing (R2);
 *  - del from a filled tail position leaves the cursor on the next element
 *    (z_after_delete): GET_CURRENT reads the next key's first duplicate
 *    and del acts on that re-read position;
 *  - MDBX_NEXT from every one of these states lands on the next key's
 *    first duplicate, or MDBX_NOTFOUND at the end of the table.
 *
 * Axes: plain DUPSORT, DUPSORT+DUPFIXED, DUPSORT+DUPFIXED+INTEGERDUP and
 * DUPSORT+REVERSEDUP (the reverse data comparator), so both the sub-page
 * and the nested-tree dupset storages are exercised on any page size. */

namespace {

constexpr unsigned kDups = 8;
const char *const kDb = "delete-after-tail.mdbx";

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

struct cursor_ptr {
  MDBX_cursor *mc = nullptr;
  ~cursor_ptr() {
    if (mc)
      mdbx_cursor_close(mc);
  }
};

struct axis {
  const char *table;
  MDBX_db_flags_t flags;
  bool integer; /* INTEGERDUP: 4-byte integer duplicates */
};

/* The REVERSEDUP axis runs the whole scenario through the reverse data
 * comparator; for values differing only in their final character the
 * reverse-string order coincides with the ascending one. */
const axis kAxes[] = {
    {"plain", MDBX_DUPSORT, false},
    {"dupfixed", static_cast<MDBX_db_flags_t>(MDBX_DUPSORT | MDBX_DUPFIXED), false},
    {"intdup", static_cast<MDBX_db_flags_t>(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP), true},
    {"revdup", static_cast<MDBX_db_flags_t>(MDBX_DUPSORT | MDBX_REVERSEDUP), false},
};

MDBX_val dup_value(const axis &a, unsigned i, void *storage) {
  if (a.integer) {
    static_assert(sizeof(uint32_t) == 4, "unexpected uint32_t size");
    *static_cast<uint32_t *>(storage) = 1000 + i;
    return MDBX_val{storage, sizeof(uint32_t)};
  }
  if (a.flags & MDBX_DUPFIXED) {
    std::memset(storage, 'x', 8);
    static_cast<char *>(storage)[7] = static_cast<char>('0' + i);
    return MDBX_val{storage, 8};
  }
  const std::string v = "value-" + std::to_string(i);
  std::memcpy(storage, v.data(), v.size());
  return MDBX_val{storage, v.size()};
}

bool values_equal(const axis &a, const MDBX_val &got, unsigned index) {
  char buf[24];
  MDBX_val want;
  if (a.integer) {
    uint32_t v = 1000 + index;
    std::memcpy(buf, &v, sizeof v);
    want = MDBX_val{buf, sizeof v};
  } else if (a.flags & MDBX_DUPFIXED) {
    std::memset(buf, 'x', 8);
    buf[7] = static_cast<char>('0' + index);
    want = MDBX_val{buf, 8};
  } else {
    const int n = std::snprintf(buf, sizeof buf, "value-%u", index);
    want = MDBX_val{buf, size_t(n)};
  }
  return want.iov_len == got.iov_len && std::memcmp(want.iov_base, got.iov_base, want.iov_len) == 0;
}

MDBX_val key_value(char *storage, unsigned k) {
  std::snprintf(storage, 8, "k%02u", k);
  return MDBX_val{storage, std::strlen(storage)};
}

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val &key, const MDBX_val &value) {
  ASSERT_EQ(mdbx_put(txn, dbi, const_cast<MDBX_val *>(&key), const_cast<MDBX_val *>(&value), MDBX_APPENDDUP),
            MDBX_SUCCESS);
}

} // namespace

TEST(delete_after_tail, dupsort_contract) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 8), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  /* one table per axis, five keys x kDups duplicates each */
  {
    txn_ptr txn;
    ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    for (const axis &a : kAxes) {
      MDBX_dbi dbi;
      ASSERT_EQ(mdbx_dbi_open(txn.txn, a.table, static_cast<MDBX_db_flags_t>(a.flags | MDBX_CREATE), &dbi),
                MDBX_SUCCESS);
      for (unsigned k = 0; k < 5; ++k) {
        char kbuf[8];
        const MDBX_val key = key_value(kbuf, k);
        for (unsigned i = 0; i < kDups; ++i) {
          uint64_t storage = 0;
          must_put(txn.txn, dbi, key, dup_value(a, i, &storage));
        }
      }
    }
    ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  }

  for (const axis &a : kAxes) {
    /* --- shape 1: consume NEXT_DUP past the tail, then the no-op dels ---- */
    {
      txn_ptr txn;
      ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn.txn), MDBX_SUCCESS);
      MDBX_dbi dbi;
      ASSERT_EQ(mdbx_dbi_open(txn.txn, a.table, a.flags, &dbi), MDBX_SUCCESS);
      MDBX_stat stat_before;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_before, sizeof stat_before), MDBX_SUCCESS);

      cursor_ptr mc;
      ASSERT_EQ(mdbx_cursor_open(txn.txn, dbi, &mc.mc), MDBX_SUCCESS);
      char kbuf[8];
      MDBX_val key = key_value(kbuf, 2), data;
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_SET_KEY), MDBX_SUCCESS);
      EXPECT_TRUE(values_equal(a, data, 0));

      /* consume the whole dup bush: the final NEXT_DUP fails MDBX_NOTFOUND
       * and leaves the past-the-inner-tail state */
      unsigned steps = 1;
      for (; steps < kDups; ++steps) {
        const int rc = mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT_DUP);
        ASSERT_EQ(MDBX_SUCCESS, rc);
        EXPECT_TRUE(values_equal(a, data, steps));
      }
      ASSERT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT_DUP));

      /* the eof-soft inner position is documented to still read the tail
       * row (R3): GET_CURRENT returns the last duplicate ... */
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_GET_CURRENT), MDBX_SUCCESS);
      EXPECT_TRUE(values_equal(a, data, kDups - 1));
      /* ... and del acts on it: the tail duplicate is removed */
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));
      /* the settled z_inner_tail_deleted state makes del a no-op SUCCESS */
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));

      /* NEXT deterministically lands on the next key's first duplicate */
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT), MDBX_SUCCESS);
      char want_key[8];
      const MDBX_val want = key_value(want_key, 3);
      EXPECT_EQ(want.iov_len, key.iov_len);
      EXPECT_EQ(0, std::memcmp(want.iov_base, key.iov_base, key.iov_len));
      EXPECT_TRUE(values_equal(a, data, 0));
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_GET_CURRENT), MDBX_SUCCESS);
      EXPECT_TRUE(values_equal(a, data, 0));

      MDBX_stat stat_after;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_after, sizeof stat_after), MDBX_SUCCESS);
      EXPECT_EQ(stat_before.ms_entries - 1, stat_after.ms_entries)
          << "exactly the tail duplicate is removed, the repeated del is a no-op";
    }

    /* --- shape 2: delete the tail duplicate itself ----------------------- */
    {
      txn_ptr txn;
      ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn.txn), MDBX_SUCCESS);
      MDBX_dbi dbi;
      ASSERT_EQ(mdbx_dbi_open(txn.txn, a.table, a.flags, &dbi), MDBX_SUCCESS);
      MDBX_stat stat_before;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_before, sizeof stat_before), MDBX_SUCCESS);

      cursor_ptr mc;
      ASSERT_EQ(mdbx_cursor_open(txn.txn, dbi, &mc.mc), MDBX_SUCCESS);
      char kbuf[8];
      MDBX_val key = key_value(kbuf, 2), data;
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_SET_KEY), MDBX_SUCCESS);
      for (unsigned steps = 1; steps < kDups; ++steps)
        ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT_DUP), MDBX_SUCCESS);
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_GET_CURRENT), MDBX_SUCCESS);
      EXPECT_TRUE(values_equal(a, data, kDups - 1));

      /* the tail row itself is a valid position: deleting it succeeds */
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));
      /* post-delete the cursor is on the next element (z_after_delete):
       * GET_CURRENT reads it ... */
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_GET_CURRENT), MDBX_SUCCESS);
      char want_key[8];
      const MDBX_val want = key_value(want_key, 3);
      EXPECT_EQ(0, std::memcmp(want.iov_base, key.iov_base, key.iov_len));
      EXPECT_TRUE(values_equal(a, data, 0));
      /* ... and del acts on the re-read position */
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));

      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT), MDBX_SUCCESS);
      EXPECT_EQ(0, std::memcmp(want.iov_base, key.iov_base, key.iov_len));
      EXPECT_TRUE(values_equal(a, data, 1));

      MDBX_stat stat_after;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_after, sizeof stat_after), MDBX_SUCCESS);
      EXPECT_EQ(stat_before.ms_entries - 2, stat_after.ms_entries)
          << "the tail duplicate and the re-read next element are removed";
    }

    /* --- shape 3: the same at the very end of the table ------------------ */
    {
      txn_ptr txn;
      ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn.txn), MDBX_SUCCESS);
      MDBX_dbi dbi;
      ASSERT_EQ(mdbx_dbi_open(txn.txn, a.table, a.flags, &dbi), MDBX_SUCCESS);
      MDBX_stat stat_before;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_before, sizeof stat_before), MDBX_SUCCESS);

      cursor_ptr mc;
      ASSERT_EQ(mdbx_cursor_open(txn.txn, dbi, &mc.mc), MDBX_SUCCESS);
      char kbuf[8];
      MDBX_val key = key_value(kbuf, 4), data;
      ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_SET_KEY), MDBX_SUCCESS);
      for (unsigned steps = 1; steps < kDups; ++steps)
        ASSERT_EQ(mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT_DUP), MDBX_SUCCESS);
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));
      EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(mc.mc, MDBX_CURRENT));
      /* no next key: MDBX_NOTFOUND at the end of the table */
      EXPECT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(mc.mc, &key, &data, MDBX_NEXT));
      /* known contract gap set-range-past-end-dual-encoding (SET_RANGE past-end dual encoding): the
       * exact GET_CURRENT outcome from the past-end state depends on the
       * internal path that produced it - deliberately not asserted here */
      MDBX_stat stat_after;
      ASSERT_EQ(mdbx_dbi_stat(txn.txn, dbi, &stat_after, sizeof stat_after), MDBX_SUCCESS);
      EXPECT_EQ(stat_before.ms_entries - 1, stat_after.ms_entries)
          << "at the end of the table the repeated del is a no-op";
    }
  }
}
