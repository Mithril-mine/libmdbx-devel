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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/600
//
// A page is moved into a free page below the "goal edge" - the number of pages that stay in the database. A moving page
// takes a place for itself and for a copy of each of its parents that has no new place yet. When the free pages below
// the goal edge were used up, the walk stopped there, spent idle cycles, reported the stop as MDBX_defrag_error and
// left the datafile larger than necessary, although free pages remained between the goal edge and the data and every
// move was still toward the beginning of the database; a repeated call could not change anything.
//
// Now, once the walk has stalled for two cycles at such a page, the page also moves into a free page between the goal
// edge and itself, and a large page into a run of such pages when no run can be formed below the goal edge. When every
// page has a number below the goal edge while free pages remain in holes lower down, the walk goes down to them too: it
// used to repeat idle cycles without end in that state. What no move can reclaim, since the last pages lack free pages
// for themselves and for the copies of their parents, or since no run can be formed for a large page, is a limit of the
// structure: it is reported as MDBX_RESULT_TRUE without MDBX_defrag_error, and the page is named.
//
// The layouts reach these states at every page size tried (256 bytes to 64 KiB): values of pagesize/22 or pagesize/11
// bytes, every second record deleted.

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

const char *const kPath = "issue0600.mdbx";
const char *const kTable = "main";
constexpr uint32_t kRecords = 800;
/* The residual of free pages the caller accepts: the last pages above the goal edge need a copy of the whole chain of
 * their parents, so a few pages always stay free. */
constexpr intptr_t kAcceptableSlack = 20;

/* What the table holds: `records` records with values of `value_bytes`, every `large_every`-th one of `large_bytes`. */
struct layout {
  uint32_t records, large_every;
  size_t value_bytes, large_bytes;
};

/* The layouts are built for the page size of the host: with values of pagesize/22 bytes a page takes the same number
 * of records at every page size. `large_pages` and a quarter is the length of a large value. */
layout make_layout(uint32_t records, uint32_t large_every, size_t large_pages) {
  const size_t page_bytes = mdbx_default_pagesize();
  layout l{records, large_every, page_bytes / 22, 0};
  if (large_pages)
    l.large_bytes = page_bytes * large_pages + page_bytes / 4;
  return l;
}

char value_byte(uint32_t record, size_t offset) { return static_cast<char>('a' + (record + offset) % 26); }

size_t value_bytes_of(uint32_t record, const layout &l) {
  return (l.large_every && record % l.large_every == 0) ? l.large_bytes : l.value_bytes;
}

/* The database of a layout: `records` records, every second one deleted. */
/* EXPECT_EQ() and the answer, for the constructor of the database, where an assertion macro can not return. */
bool expect_ok(int rc) {
  EXPECT_EQ(MDBX_SUCCESS, rc);
  return rc == MDBX_SUCCESS;
}

/* The database of a layout: `records` records, every second one deleted. */
class database {
  MDBX_env *env_ = nullptr;

public:
  const layout l;
  explicit database(layout layout) : l(layout) {
    if (!expect_ok(mdbx_env_create(&env_)) || !expect_ok(mdbx_env_set_maxdbs(env_, 2)) ||
        !expect_ok(mdbx_env_open(env_, kPath, MDBX_NOSUBDIR, 0644)))
      return;

    MDBX_dbi dbi;
    {
      /* The table is created by a transaction of its own, as the other defragmentation tests do. */
      MDBX_txn *txn = nullptr;
      if (!expect_ok(mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn)))
        return;
      txn_handle_guard txn_owner(&txn);
      if (!expect_ok(mdbx_dbi_open(txn, kTable, MDBX_CREATE, &dbi)) || !expect_ok(mdbx_txn_commit(txn)))
        return;
      txn_owner.release();
    }
    {
      MDBX_txn *txn = nullptr;
      if (!expect_ok(mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn)))
        return;
      txn_handle_guard txn_owner(&txn);
      if (!expect_ok(mdbx_dbi_open(txn, kTable, MDBX_DB_DEFAULTS, &dbi)))
        return;
      std::vector<char> value((l.large_bytes > l.value_bytes) ? l.large_bytes : l.value_bytes);
      for (uint32_t i = 0; i < l.records; ++i) {
        const size_t bytes = value_bytes_of(i, l);
        for (size_t j = 0; j < bytes; ++j)
          value[j] = value_byte(i, j);
        MDBX_val key{&i, sizeof(i)}, data{value.data(), bytes};
        if (!expect_ok(mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT)))
          return;
      }
      if (expect_ok(mdbx_txn_commit(txn)))
        txn_owner.release();
    }
    {
      MDBX_txn *txn = nullptr;
      if (!expect_ok(mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn)))
        return;
      txn_handle_guard txn_owner(&txn);
      if (!expect_ok(mdbx_dbi_open(txn, kTable, MDBX_DB_DEFAULTS, &dbi)))
        return;
      for (uint32_t i = 0; i < l.records; i += 2) {
        MDBX_val key{&i, sizeof(i)};
        if (!expect_ok(mdbx_del(txn, dbi, &key, nullptr)))
          return;
      }
      if (expect_ok(mdbx_txn_commit(txn)))
        txn_owner.release();
    }
  }
  database(const database &) = delete;
  ~database() {
    if (env_)
      mdbx_env_close(env_);
  }
  MDBX_env *env() const { return env_; }

  /* Reads the table back and checks that exactly the odd records remain, each with an intact value. */
  void check() const {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn));
    txn_handle_guard txn_owner(&txn);
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, kTable, MDBX_DB_DEFAULTS, &dbi));
    MDBX_cursor *cursor = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
    std::vector<bool> seen(l.records, false);
    uint32_t count = 0;
    MDBX_val key, data;
    int rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
    while (rc == MDBX_SUCCESS && count <= l.records) {
      uint32_t record = l.records;
      EXPECT_EQ(sizeof(uint32_t), key.iov_len);
      if (key.iov_len == sizeof(uint32_t))
        std::memcpy(&record, key.iov_base, sizeof record);
      EXPECT_LT(record, l.records);
      EXPECT_EQ(1u, record % 2) << "only the odd records remain";
      if (record < l.records) {
        EXPECT_FALSE(seen[record]) << "record " << record << " is met twice";
        seen[record] = true;
        ++count;
        const size_t bytes = value_bytes_of(record, l);
        EXPECT_EQ(bytes, data.iov_len) << "record " << record;
        for (size_t j = 0; j < bytes && j < data.iov_len; ++j)
          EXPECT_EQ(value_byte(record, j), static_cast<const char *>(data.iov_base)[j]) << "record " << record;
      }
      rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
    }
    mdbx_cursor_close(cursor);
    EXPECT_EQ(MDBX_NOTFOUND, rc) << "more records than expected";
    EXPECT_EQ(l.records / 2, count);
  }
};

/* The defragmentation of the issue: no goals, no limit on the size of a cycle, and `slack` as the residual of free
 * pages the caller accepts. */
int defrag(const database &db, intptr_t slack, MDBX_defrag_result_t *result) {
  std::memset(result, 0, sizeof *result);
  return mdbx_env_defrag(db.env(), 0, 0, 0, 0, slack, 0, nullptr, nullptr, result);
}

/* No residual is accepted, and the structure of the trees allows no more pages to be moved: the defragmentation stops
 * without MDBX_defrag_error, names the page that stays, and a repeated call has no work. */
void stops_at_the_limit_of_the_structure(const database &db) {
  MDBX_defrag_result_t result;
  const int rc = defrag(db, 0, &result);
  EXPECT_EQ(MDBX_RESULT_TRUE, rc) << "stopping_reasons=" << result.stopping_reasons;
  EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error) << "a limit of the structure is no error";
  EXPECT_EQ(0u, result.stopping_reasons & ~unsigned(MDBX_defrag_large_chunk))
      << "a limit of the structure names no reason but a large page";
  EXPECT_NE(0u, result.obstructed_pgno) << "the page that can not be moved is named";
  db.check();

  /* nothing is left to move: a repeated call moves no page and reports no error; it either meets the same limit or
   * finds that the tail the first call left in the GC has been cut off and the goal is reached */
  MDBX_defrag_result_t again;
  const int again_rc = defrag(db, 0, &again);
  EXPECT_TRUE(again_rc == MDBX_SUCCESS || again_rc == MDBX_RESULT_TRUE) << "rc " << again_rc;
  EXPECT_EQ(0u, again.stopping_reasons & MDBX_defrag_error);
  EXPECT_EQ(0u, again.pages_moved) << "the first defragmentation left nothing to move";
  EXPECT_GE(result.pages_shrinked, intptr_t(0));
  EXPECT_EQ(result.pages_whole - static_cast<size_t>(result.pages_shrinked), again.pages_whole)
      << "a repeated defragmentation changes nothing";
  db.check();
}

/* The defragmentation completes, a repeated one has no work, and the data is intact. */
void defragmented_completely(const database &db) {
  MDBX_defrag_result_t result;
  const int rc = defrag(db, kAcceptableSlack, &result);
  EXPECT_EQ(MDBX_SUCCESS, rc) << "stopping_reasons=" << result.stopping_reasons;
  EXPECT_EQ(0u, result.stopping_reasons) << "a completed defragmentation names no reason to stop";
  EXPECT_GT(result.pages_moved, 0u);
  EXPECT_GT(result.pages_shrinked, intptr_t(0)) << "the datafile became smaller";
  db.check();

  /* nothing is left to move: a repeated defragmentation has no work */
  MDBX_defrag_result_t again;
  EXPECT_EQ(MDBX_SUCCESS, defrag(db, kAcceptableSlack, &again));
  EXPECT_EQ(0u, again.pages_moved) << "the first defragmentation left nothing to move";
  EXPECT_EQ(result.pages_whole - static_cast<size_t>(result.pages_shrinked), again.pages_whole);
  db.check();
}

} // namespace

TEST(issue_gh600, page_moves_into_free_slot_above_goal_edge) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  database db(make_layout(kRecords, 0, 0));
  ASSERT_NE(nullptr, db.env());
  defragmented_completely(db);
}

TEST(issue_gh600, large_page_moves_into_free_slot_above_goal_edge) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  database db(make_layout(kRecords, /*large_every*/ 4, /*large_pages*/ 1));
  ASSERT_NE(nullptr, db.env());
  defragmented_completely(db);
}

TEST(issue_gh600, large_pages_are_not_an_error) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* Every third record is a large page of six pages, and no residual is accepted. The large pages take the runs that
   * can be made for them, below the goal edge or between the goal edge and the data, and what stays is a limit of the
   * structure: the defragmentation used to report it as an error, or not to end at all. */
  database db(make_layout(/*records*/ 200, /*large_every*/ 3, /*large_pages*/ 5));
  ASSERT_NE(nullptr, db.env());
  stops_at_the_limit_of_the_structure(db);
}

TEST(issue_gh600, no_free_page_for_parents_is_not_an_error) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  database db(make_layout(/*records*/ 40, 0, 0));
  ASSERT_NE(nullptr, db.env());

  /* No residual is accepted, but the last pages have no free page left for themselves and for the copies of their
   * parents: a limit of the structure. */
  stops_at_the_limit_of_the_structure(db);
}

TEST(issue_gh600, large_pages_do_not_make_endless_cycles) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* Every third record is a large page of eight and a quarter pages, and no residual is accepted. The defragmentation
   * used to repeat idle cycles without end here: every page had a number below the goal edge, while free pages remained
   * in holes lower down, and the walk had nothing to stumble on. */
  database db(make_layout(/*records*/ 60, /*large_every*/ 3, /*large_pages*/ 8));
  ASSERT_NE(nullptr, db.env());
  stops_at_the_limit_of_the_structure(db);
}
