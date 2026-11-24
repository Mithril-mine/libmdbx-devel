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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/605
//
// Without MDBX_ENABLE_BIGFOOT the numbers of the pages retired by a transaction are stored as one value of the GC
// tree, which takes a run of adjacent pages. The commit used to reserve one page for the value, however long the list
// was, and then took the whole run from the end of the datafile, although free pages inside the data could hold it.
// With the database at its size limit, the commit failed with MDBX_MAP_FULL.
//
// The layout needs the smallest page size, where the list of the retired pages takes three pages; with larger pages
// the list fits on one page, and there is nothing to reserve. The database holds interleaved one-record pages of two
// key ranges; one range is retired by a single transaction while a run of three free pages exists inside the data,
// and records that are never deleted keep the pages of the GC away from the end of the datafile.

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

const char *const kPath = "issue0605.mdbx";
const char *const kTable = "main";
constexpr uint32_t kPairs = 150;
/* Records with keys above all the others that are never deleted: the pages that the GC frees and takes again at
 * every commit would give room at the end of the datafile without them. */
constexpr uint32_t kAnchors = 92;
constexpr uint32_t kMiddle = kPairs / 2;

uint32_t key_a(uint32_t i) { return 2 * i; }
uint32_t key_b(uint32_t i) { return 2 * i + 1; }
uint32_t key_anchor(uint32_t i) { return 0x60000000u + i; }

struct usage {
  size_t allocated, free_pages;
};

int count_free_pages(void *ctx, const MDBX_txn *txn, uint64_t span_txnid, size_t span_pgno, size_t span_length,
                     bool span_is_reclaimable) noexcept {
  (void)txn;
  (void)span_txnid;
  (void)span_pgno;
  if (span_is_reclaimable)
    *static_cast<size_t *>(ctx) += span_length;
  return MDBX_SUCCESS; /* go on with the next span */
}

/* The pages the database has and the pages the GC can give for a new record. */
usage usage_of(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  txn_handle_guard txn_owner(&txn);
  MDBX_gc_info_t info;
  std::memset(&info, 0, sizeof info);
  size_t free_pages = 0;
  const int rc = mdbx_gc_info(txn, &info, sizeof info, count_free_pages, &free_pages);
  EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND) << "rc=" << rc;
  return {info.pages_allocated, free_pages};
}

class database {
  MDBX_env *env_ = nullptr;
  std::vector<char> value_;

public:
  database() {
    if (!expect_ok(mdbx_env_create(&env_)))
      return;
    if (!expect_ok(mdbx_env_set_geometry(env_, -1, -1, -1, -1, -1, mdbx_limits_pgsize_min())) ||
        !expect_ok(mdbx_env_set_maxdbs(env_, 2)) || !expect_ok(mdbx_env_open(env_, kPath, MDBX_NOSUBDIR, 0644)))
      return;

    MDBX_envinfo info;
    std::memset(&info, 0, sizeof info);
    if (!expect_ok(mdbx_env_info_ex(env_, nullptr, &info, sizeof info)))
      return;
    /* one record per page */
    value_.assign(static_cast<size_t>(info.mi_dxb_pagesize) * 6 / 10, 'x');
  }
  database(const database &) = delete;
  ~database() {
    if (env_)
      mdbx_env_close(env_);
  }

  /* EXPECT_EQ() and its answer for the constructor, where an assertion macro can not return. */
  static bool expect_ok(int rc) {
    EXPECT_EQ(MDBX_SUCCESS, rc);
    return rc == MDBX_SUCCESS;
  }

  MDBX_env *env() const { return env_; }

  void put(MDBX_txn *txn, MDBX_dbi dbi, uint32_t key) {
    MDBX_val k{&key, sizeof key}, data{value_.data(), value_.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &data, MDBX_UPSERT));
  }

  static void del(MDBX_txn *txn, MDBX_dbi dbi, uint32_t key) {
    MDBX_val k{&key, sizeof key};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_del(txn, dbi, &k, nullptr));
  }

  /* A write transaction with the table open; `create` opens the table for the first time. */
  template <typename body_t> void write(bool create, body_t body) {
    MDBX_txn *txn = nullptr;
    ASSERT_NE(nullptr, env_);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn));
    txn_handle_guard txn_owner(&txn);
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, kTable, create ? MDBX_CREATE : MDBX_DB_DEFAULTS, &dbi));
    body(txn, dbi);
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    txn_owner.release();
  }

  /* The records that stay: B at kMiddle and kMiddle + 2, A at kMiddle + 3, and the anchors. */
  void check() const {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn));
    txn_handle_guard txn_owner(&txn);
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, kTable, MDBX_DB_DEFAULTS, &dbi));
    MDBX_stat stat;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, dbi, &stat, sizeof stat));
    EXPECT_EQ(3u + kAnchors, stat.ms_entries);
    for (uint32_t i = 0; i < kPairs; ++i) {
      uint32_t key = key_a(i);
      MDBX_val k{&key, sizeof key}, data;
      const bool a_stays = (i == kMiddle + 3), b_stays = (i == kMiddle || i == kMiddle + 2);
      EXPECT_EQ(a_stays ? MDBX_SUCCESS : MDBX_NOTFOUND, mdbx_get(txn, dbi, &k, &data)) << "record A" << i;
      key = key_b(i);
      EXPECT_EQ(b_stays ? MDBX_SUCCESS : MDBX_NOTFOUND, mdbx_get(txn, dbi, &k, &data)) << "record B" << i;
    }
  }
};

} // namespace

TEST(issue_gh605, retired_pages_get_their_record_inside_the_data) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  database db;
  ASSERT_NE(nullptr, db.env());

  db.write(true, [&](MDBX_txn *txn, MDBX_dbi dbi) {
    for (uint32_t i = 0; i < kPairs; ++i) {
      db.put(txn, dbi, key_a(i));
      db.put(txn, dbi, key_b(i));
    }
    for (uint32_t i = 0; i < kAnchors; ++i)
      db.put(txn, dbi, key_anchor(i));
  });

  /* all pages of B but three, and one page of A */
  db.write(false, [&](MDBX_txn *txn, MDBX_dbi dbi) {
    for (uint32_t i = 0; i < kPairs; ++i)
      if (i < kMiddle || i > kMiddle + 2)
        database::del(txn, dbi, key_b(i));
    database::del(txn, dbi, key_a(kMiddle));
  });

  /* three adjacent pages */
  db.write(false, [&](MDBX_txn *txn, MDBX_dbi dbi) {
    database::del(txn, dbi, key_a(kMiddle + 1));
    database::del(txn, dbi, key_b(kMiddle + 1));
    database::del(txn, dbi, key_a(kMiddle + 2));
  });

  const usage before = usage_of(db.env());
  ASSERT_GT(before.free_pages, size_t{3}) << "free pages for the record exist inside the data";

  /* the rest of A: their numbers take three pages at the smallest page size */
  db.write(false, [&](MDBX_txn *txn, MDBX_dbi dbi) {
    for (uint32_t i = 0; i < kPairs; ++i)
      if (i < kMiddle || i > kMiddle + 3)
        database::del(txn, dbi, key_a(i));
  });

  const usage after = usage_of(db.env());
  EXPECT_GE(before.allocated, after.allocated)
      << "the record of the retired pages was taken from the end of the datafile";
  db.check();
}
