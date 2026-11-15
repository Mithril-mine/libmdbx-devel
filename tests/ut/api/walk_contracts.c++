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

/* The walk-visitor byte-accounting contract (LEM-V4-4 / INV-V4-4 of the
 * verification campaign, issue #485) observed through the public
 * mdbx_env_chk() surface: the chk engine drives the internal walk_pages()
 * visitor, asserts the per-page decomposition payload + header + unused ==
 * page size for every legal shape (any violation surfaces as a "misused"
 * problem) and feeds the per-table page/byte aggregates. On a database that
 * exercises every shape - branch and leaf pages, large/overflow chains,
 * sub-page dup bushes, nested dupsort sub-trees and dupfix leaves - the
 * check must complete with zero problems and exact per-shape page counts:
 *
 *  - the overflow chains: five values sized 2..6 pages (bulk data sized in
 *    pages, so the scenario is page-size independent) give exactly five
 *    chains spanning exactly 2+3+4+5+6 pages;
 *  - a tall enough main table has branch pages;
 *  - the dupsort table has nested sub-tree pages and a sub-page bush;
 *  - the dupfixed table has dupfix leaf pages;
 *  - no table reports empty/broken pages or lost bytes.
 *
 * The raw walk_func interface is internal (src/walk.h) and cannot be linked
 * from a test unit reliably (the amalgamated alloy build inlines the
 * engine's callees), so the byte-accounting invariant is pinned through the
 * supported API that consumes it. */

namespace {

const char *kDb = "walk-contracts.mdbx";

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

struct table_snapshot {
  std::string name;
  size_t pages_all, pages_empty, pages_broken, pages_branch, pages_leaf;
  size_t nested_branch, nested_leaf, nested_subleaf;
  size_t large_amount, large_count;
  size_t payload_bytes, lost_bytes, items;
};

struct chk_capture {
  std::vector<table_snapshot> tables;
  static int on_table_conclude(MDBX_chk_context_t *, const MDBX_chk_table_t *table, MDBX_cursor *, int err) {
    chk_capture *self = current;
    if (err != MDBX_SUCCESS || !table)
      return err;
    table_snapshot snap;
    snap.name = std::string(static_cast<const char *>(table->name.iov_base), table->name.iov_len);
    snap.pages_all = table->pages.all;
    snap.pages_empty = table->pages.empty;
    snap.pages_broken = table->pages.broken;
    snap.pages_branch = table->pages.branch;
    snap.pages_leaf = table->pages.leaf;
    snap.nested_branch = table->pages.nested_branch;
    snap.nested_leaf = table->pages.nested_leaf;
    snap.nested_subleaf = table->pages.nested_subleaf;
    snap.large_amount = table->histogram.large_pages.amount;
    snap.large_count = table->histogram.large_pages.count;
    snap.payload_bytes = table->payload_bytes;
    snap.lost_bytes = table->lost_bytes;
    self->tables.push_back(snap);
    return MDBX_SUCCESS;
  }
  static chk_capture *current;
};
chk_capture *chk_capture::current = nullptr;

const table_snapshot *find(const std::vector<table_snapshot> &tables, const std::string &name) {
  for (const auto &t : tables)
    if (t.name == name)
      return &t;
  return nullptr;
}

} // namespace

TEST(walk_contracts, shape_accounting_exact) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644));

  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env.get(), nullptr, &info, sizeof info));
  const uint32_t ps = info.mi_dxb_pagesize;
  ASSERT_GE(ps, 256u);

  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi mix, dupsort, dupfix;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "mix", MDBX_CREATE, &mix));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "dupsort", MDBX_DUPSORT | MDBX_CREATE, &dupsort));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "dupfix", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dupfix));

    std::vector<char> val(6 * ps + 64);
    /* five overflow chains of exactly 2..6 pages: bytes2pgno(PAGEHDRSZ-1+ds)+1
     * with ds = k*ps-100 gives exactly k pages for every k */
    for (unsigned k = 0; k < 5; ++k) {
      char key[16];
      const int klen = std::snprintf(key, sizeof key, "big-%u", k);
      MDBX_val kv = {key, size_t(klen)}, vv = {val.data(), size_t(k + 2) * ps - 100};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, mix, &kv, &vv, MDBX_UPSERT));
    }
    /* enough small keys for a multi-level tree on any page size: the count
     * scales with the page size (a fixed count would fit a single 64 KiB
     * leaf and never produce branch pages) */
    const unsigned key_count = ps / 34 * 4;
    for (unsigned k = 0; k < key_count; ++k) {
      char key[16];
      const int klen = std::snprintf(key, sizeof key, "key-%04u", k);
      MDBX_val kv = {key, size_t(klen)}, vv = {val.data(), 16};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, mix, &kv, &vv, MDBX_UPSERT));
    }
    /* a dupset large enough to become a nested sub-tree (N_TREE) */
    MDBX_val bush_big = {const_cast<char *>("bush-big"), 8};
    const unsigned bush_big_count = ps / 4;
    for (unsigned i = 0; i < bush_big_count; ++i) {
      char vb[24];
      const int vlen = std::snprintf(vb, sizeof vb, "value-%06u", i);
      MDBX_val dv = {vb, size_t(vlen)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dupsort, &bush_big, &dv, MDBX_APPENDDUP));
    }
    /* and a small bush stored as a sub-page (N_DUP) */
    MDBX_val bush_small = {const_cast<char *>("bush-small"), 10};
    for (unsigned i = 0; i < 3; ++i) {
      char vb[8];
      const int vlen = std::snprintf(vb, sizeof vb, "s-%u", i);
      MDBX_val dv = {vb, size_t(vlen)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dupsort, &bush_small, &dv, MDBX_APPENDDUP));
    }
    /* dupfix leaves: fixed-size duplicates under several keys; the values
     * are 0..255 so their little-endian byte-string order stays ascending
     * (a wider range would break the APPENDDUP order at 0x100) */
    const unsigned dupfix_per_key = ps / 32 > 256 ? 256u : ps / 32;
    for (unsigned k = 0; k < 8; ++k) {
      char key[16];
      const int klen = std::snprintf(key, sizeof key, "df-%02u", k);
      MDBX_val kv = {key, size_t(klen)};
      for (unsigned i = 0; i < dupfix_per_key; ++i) {
        uint32_t v = i; /* LE byte-string order stays ascending below 0x100 */
        MDBX_val dv = {&v, sizeof v};
        ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dupfix, &kv, &dv, MDBX_APPENDDUP));
      }
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  chk_capture capture;
  chk_capture::current = &capture;
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb);
  cb.table_conclude = &chk_capture::on_table_conclude;
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  const int rc = mdbx_env_chk(env.get(), &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  chk_capture::current = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, rc);
  EXPECT_EQ(0u, ctx.result.total_problems)
      << "any per-page decomposition or accounting violation would surface as a problem";

  const table_snapshot *mix = find(capture.tables, "mix");
  ASSERT_NE(nullptr, mix);
  EXPECT_EQ(5u, mix->large_count) << "five overflow chains";
  EXPECT_EQ(2u + 3u + 4u + 5u + 6u, mix->large_amount) << "the exact chain span pages";
  EXPECT_GE(mix->pages_branch, 1u) << "a tall enough tree has branch pages";
  EXPECT_GE(mix->pages_leaf, 1u);
  EXPECT_EQ(0u, mix->pages_empty);
  EXPECT_EQ(0u, mix->pages_broken);
  EXPECT_EQ(0u, mix->lost_bytes);
  EXPECT_GT(mix->payload_bytes, 0u);

  const table_snapshot *dupsort = find(capture.tables, "dupsort");
  ASSERT_NE(nullptr, dupsort);
  EXPECT_TRUE(dupsort->nested_branch + dupsort->nested_leaf > 0) << "the nested dupset sub-tree is walked";
  EXPECT_GE(dupsort->nested_subleaf, 1u) << "the sub-page bush is accounted";
  EXPECT_EQ(0u, dupsort->pages_broken);
  EXPECT_EQ(0u, dupsort->lost_bytes);

  const table_snapshot *dupfix = find(capture.tables, "dupfix");
  ASSERT_NE(nullptr, dupfix);
  EXPECT_GE(dupfix->pages_leaf, 1u) << "dupfix leaf pages are accounted";
  EXPECT_EQ(0u, dupfix->pages_broken);
  EXPECT_EQ(0u, dupfix->lost_bytes);
}
