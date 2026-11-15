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

/* Deterministic contract corners of mdbx_cursor_delete_range() and
 * mdbx_cursor_bunch_delete() on DUPSORT tables - the verified-green
 * behaviors, complementary to the randomized model of bunches_removal and
 * the simple cases of defrag-cursors:
 *
 *  - a range inside one dup bush cuts exactly the covered duplicates
 *    (end_including honored);
 *  - a range across keys removes full bushes in the middle and the covered
 *    parts at the edges; NULL begin/end mean the first/last item;
 *  - the argument contract: both-NULL is EINVAL, an unpositioned cursor is
 *    ENODATA, cursors of different tables are EINVAL;
 *  - a neighbor cursor positioned inside/near the deleted range survives
 *    the cutoff and stays iterable to the exact remaining model
 *    (the SAFE-A restore of the internal clone list).
 *
 * known contract gap V-029 (SET_RANGE past-end leaves two encodings of the
 * same logical state) and V-030 (a TO_KEY_EQUAL miss fills the cursor on
 * the neighbor record): range endpoints here are always positioned on real
 * pairs via SET_KEY/GET_BOTH, so the path-dependent follow-ups of those
 * states are deliberately not asserted. */

namespace {

constexpr unsigned kKeys = 10, kDups = 5;
const char *kDb = "delete-range.mdbx";

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
  MDBX_cursor **operator&() { return &mc; }
};

MDBX_val key_of(char *buf, unsigned k) {
  std::snprintf(buf, 8, "k%02u", k);
  return MDBX_val{buf, std::strlen(buf)};
}

MDBX_val dup_of(char *buf, unsigned v) {
  std::snprintf(buf, 8, "v%u", v);
  return MDBX_val{buf, std::strlen(buf)};
}

/* positions the cursor exactly on (key, dup) via GET_BOTH */
void must_position(MDBX_cursor *mc, unsigned k, unsigned v) {
  char kb[8], vb[8];
  MDBX_val key = key_of(kb, k), dup = dup_of(vb, v);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(mc, &key, &dup, MDBX_GET_BOTH));
}

void fill_table(MDBX_txn *txn, MDBX_dbi dbi) {
  for (unsigned k = 0; k < kKeys; ++k) {
    char kb[8];
    const MDBX_val key = key_of(kb, k);
    for (unsigned v = 0; v < kDups; ++v) {
      char vb[8];
      MDBX_val dup = dup_of(vb, v);
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, const_cast<MDBX_val *>(&key), &dup, MDBX_APPENDDUP));
    }
  }
}

/* the expected remaining content as "k,v" strings */
using model_t = std::vector<std::pair<unsigned, unsigned>>;

model_t model_all() {
  model_t m;
  for (unsigned k = 0; k < kKeys; ++k)
    for (unsigned v = 0; v < kDups; ++v)
      m.emplace_back(k, v);
  return m;
}

void expect_content(MDBX_txn *txn, MDBX_dbi dbi, const model_t &model) {
  MDBX_cursor *mc = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &mc));
  MDBX_val key, data;
  size_t i = 0;
  int rc = mdbx_cursor_get(mc, &key, &data, MDBX_FIRST);
  while (rc == MDBX_SUCCESS) {
    ASSERT_LT(i, model.size());
    char kb[8], vb[8];
    const MDBX_val want_key = key_of(kb, model[i].first);
    const MDBX_val want_dup = dup_of(vb, model[i].second);
    EXPECT_EQ(0, std::memcmp(want_key.iov_base, key.iov_base, key.iov_len));
    EXPECT_EQ(want_key.iov_len, key.iov_len);
    EXPECT_EQ(0, std::memcmp(want_dup.iov_base, data.iov_base, data.iov_len));
    EXPECT_EQ(want_dup.iov_len, data.iov_len);
    ++i;
    rc = mdbx_cursor_get(mc, &key, &data, MDBX_NEXT);
  }
  mdbx_cursor_close(mc);
  ASSERT_EQ(MDBX_NOTFOUND, rc);
  EXPECT_EQ(model.size(), i);
}

model_t model_drop_range(model_t m, unsigned k1, unsigned v1, unsigned k2, unsigned v2, bool end_including) {
  model_t out;
  for (const auto &kv : m) {
    const auto row = [](std::pair<unsigned, unsigned> p) { return std::pair<uint64_t, uint64_t>(p.first, p.second); };
    const auto begin = row({k1, v1});
    const auto end = row({k2, v2});
    const auto cur = row(kv);
    const bool in_range = end_including ? (cur >= begin && cur <= end) : (cur >= begin && cur < end);
    if (!in_range)
      out.push_back(kv);
  }
  return out;
}

} // namespace

TEST(delete_range_contracts, dupsort_axes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* DUPSORT = 0x04, DUPSORT|DUPFIXED = 0x14: one database file per axis */
  const env_files_guard guard({kDb, "dr-00000004.mdbx", "dr-00000014.mdbx"});
  const MDBX_db_flags_t axes[] = {MDBX_DUPSORT, static_cast<MDBX_db_flags_t>(MDBX_DUPSORT | MDBX_DUPFIXED)};

  for (const MDBX_db_flags_t flags : axes) {
    /* a fresh database per axis; every sub-case re-fills it so the counts
     * stay independent */
    char dbname[16];
    std::snprintf(dbname, sizeof dbname, "dr-%08x", static_cast<unsigned>(flags));
    const std::string dbpath = std::string(dbname) + ".mdbx";
    mdbx_env_delete(dbpath.c_str(), MDBX_ENV_JUST_DELETE);

    env_ptr env;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 4));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), dbpath.c_str(), MDBX_NOSUBDIR, 0644));

    auto refill = [&](MDBX_dbi *dbi) {
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", static_cast<MDBX_db_flags_t>(flags | MDBX_CREATE), dbi));
      /* sub-cases share the axis database: purge the previous content,
       * since re-appending an exact duplicate is MDBX_EKEYMISMATCH */
      ASSERT_EQ(MDBX_SUCCESS, mdbx_drop(txn.txn, *dbi, false));
      fill_table(txn.txn, *dbi);
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    };

    /* --- an inner-bush range, inclusive end ---------------------------- */
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr begin, end;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &begin.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &end.mc));
      must_position(begin.mc, 3, 1);
      must_position(end.mc, 3, 3);
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(begin.mc, end.mc, true, &affected));
      EXPECT_EQ(3u, affected);
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 3, 1, 3, 3, true));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }

    /* --- the same range with an exclusive end -------------------------- */
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr begin, end;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &begin.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &end.mc));
      must_position(begin.mc, 3, 1);
      must_position(end.mc, 3, 3);
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(begin.mc, end.mc, false, &affected));
      EXPECT_EQ(2u, affected) << "the end cursor's item must survive an exclusive end";
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 3, 1, 3, 3, false));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }

    /* --- a range across keys with partial bushes at the edges ---------- */
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr begin, end;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &begin.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &end.mc));
      must_position(begin.mc, 2, 3);
      must_position(end.mc, 5, 1);
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(begin.mc, end.mc, true, &affected));
      EXPECT_EQ(2u + 2 * kDups + 2u, affected);
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 2, 3, 5, 1, true));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }

    /* --- the NULL-sided forms ------------------------------------------ */
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr end;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &end.mc));
      must_position(end.mc, 1, 1);
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(nullptr, end.mc, true, &affected));
      EXPECT_EQ(kDups + 2u, affected) << "NULL begin = the first item";
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 0, 0, 1, 1, true));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr begin;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &begin.mc));
      must_position(begin.mc, 8, 2);
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(begin.mc, nullptr, true, &affected));
      EXPECT_EQ(3u + kDups, affected) << "NULL end = the last item";
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 8, 2, kKeys - 1, kDups - 1, true));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }

    /* --- the refusal contract ------------------------------------------ */
    {
      MDBX_dbi dbi, other;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "other", MDBX_DUPSORT | MDBX_CREATE, &other));
      uint64_t affected = 12345;
      EXPECT_EQ(MDBX_EINVAL, mdbx_cursor_delete_range(nullptr, nullptr, true, &affected))
          << "both sides NULL is refused";
      cursor_ptr fresh, positioned, foreign;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &fresh.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &positioned.mc));
      must_position(positioned.mc, 4, 2);
      EXPECT_EQ(MDBX_ENODATA, mdbx_cursor_delete_range(fresh.mc, positioned.mc, true, &affected))
          << "an unpositioned cursor is refused";
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, other, &foreign.mc));
      { /* position the foreign cursor: the unpositioned check (ENODATA)
         * runs before the same-table check, so only a pointed cursor of a
         * different table reaches the EINVAL branch */
        MDBX_val k = {(void *)"z", 1}, v = {(void *)"z", 1};
        ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn.txn, other, &k, &v, MDBX_UPSERT));
        ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(foreign.mc, &k, &v, MDBX_SET_KEY));
      }
      EXPECT_EQ(MDBX_EINVAL, mdbx_cursor_delete_range(foreign.mc, positioned.mc, true, &affected))
          << "cursors of different tables are refused";
      /* NOT asserted: a bunch action on an unpositioned cursor returns
       * MDBX_ENODATA, but on assert-enabled builds the refusal poisons the
       * transaction cursor state and the NEXT range operation aborts on the
       * tree_cutoff_range() invariant (assert at src/tree-cutoff.c:513;
       * release builds proceed). Reproduced with an unpositioned cursor,
       * MDBX_DELETE_AFTER_INCLUDING, then any later delete_range - the
       * scenario is excluded here until the library side is fixed; see the
       * finding in the PR description (SAFE-A restore family). */
      expect_content(txn.txn, dbi, model_all()); /* the refused calls removed nothing */
    }

    /* --- a neighbor cursor survives the cutoff ------------------------- */
    {
      MDBX_dbi dbi;
      refill(&dbi);
      txn_ptr txn;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, "t", flags, &dbi));
      cursor_ptr begin, end, neighbor;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &begin.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &end.mc));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &neighbor.mc));
      must_position(begin.mc, 1, 4);
      must_position(end.mc, 4, 0);
      must_position(neighbor.mc, 3, 2); /* inside the doomed range */
      uint64_t affected = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_delete_range(begin.mc, end.mc, true, &affected));
      EXPECT_EQ(1u + 2 * kDups + 1u, affected);
      /* the neighbor cursor stays iterable: every visited pair belongs to
       * the remaining content (the fully-deleted keys never reappear) and
       * the scan deterministically reaches the end */
      MDBX_val key, data;
      int rc = mdbx_cursor_get(neighbor.mc, &key, &data, MDBX_GET_CURRENT);
      EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND || rc == MDBX_ENODATA)
          << "the neighbor cursor must stay usable, got " << rc;
      unsigned visited = 0;
      rc = mdbx_cursor_get(neighbor.mc, &key, &data, MDBX_NEXT);
      while (rc == MDBX_SUCCESS) {
        ++visited;
        const std::string k(static_cast<const char *>(key.iov_base), key.iov_len);
        EXPECT_NE("k02", k);
        EXPECT_NE("k03", k) << "deleted keys must not resurface";
        rc = mdbx_cursor_get(neighbor.mc, &key, &data, MDBX_NEXT);
      }
      EXPECT_EQ(MDBX_NOTFOUND, rc);
      EXPECT_LE(visited, 6u * kDups) << "the scan cannot exceed the remaining items";
      expect_content(txn.txn, dbi, model_drop_range(model_all(), 1, 4, 4, 0, true));
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
    }
  }
}

TEST(delete_range_contracts, bunch_actions) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  auto refill = [&](MDBX_dbi *dbi) {
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DUPSORT | MDBX_CREATE, dbi));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_drop(txn.txn, *dbi, false));
    fill_table(txn.txn, *dbi);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  };

  /* AFTER_INCLUDING: from the current pair through the end of the table */
  {
    MDBX_dbi dbi;
    refill(&dbi);
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DUPSORT, &dbi));
    cursor_ptr mc;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &mc.mc));
    must_position(mc.mc, 5, 0);
    uint64_t affected = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_bunch_delete(mc.mc, MDBX_DELETE_AFTER_INCLUDING, &affected));
    EXPECT_EQ(5 * kDups, affected);
    expect_content(txn.txn, dbi, model_drop_range(model_all(), 5, 0, kKeys - 1, kDups - 1, true));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }

  /* BEFORE_EXCLUDING: everything strictly before the current pair */
  {
    MDBX_dbi dbi;
    refill(&dbi);
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DUPSORT, &dbi));
    cursor_ptr mc;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &mc.mc));
    must_position(mc.mc, 5, 2);
    uint64_t affected = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_bunch_delete(mc.mc, MDBX_DELETE_BEFORE_EXCLUDING, &affected));
    EXPECT_EQ(5 * kDups + 2u, affected);
    expect_content(txn.txn, dbi, model_drop_range(model_all(), 0, 0, 5, 1, true));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }

  /* CURRENT_MULTIVAL_ALL on a mid bush removes the whole key */
  {
    MDBX_dbi dbi;
    refill(&dbi);
    txn_ptr txn;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn.txn, nullptr, MDBX_DUPSORT, &dbi));
    cursor_ptr mc;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn.txn, dbi, &mc.mc));
    must_position(mc.mc, 3, 2);
    uint64_t affected = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_bunch_delete(mc.mc, MDBX_DELETE_CURRENT_MULTIVAL_ALL, &affected));
    EXPECT_EQ(kDups, affected);
    expect_content(txn.txn, dbi, model_drop_range(model_all(), 3, 0, 3, kDups - 1, true));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn.release()));
  }
}
