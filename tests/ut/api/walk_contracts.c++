/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", WITHOUT WARRANTY OF ANY KIND, express or implied, including but not
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
 * verification campaign, issue #485) on a database that exercises every
 * shape - branch and leaf pages, large/overflow chains, sub-page dup
 * bushes, nested dupsort sub-trees and dupfix leaves - with the bulk data
 * sized in pages, so the scenario is page-size independent:
 *
 *  - raw_visitor_byte_accounting drives the internal walk engine directly
 *    through the test-only mdbx_injection_walk_pages() (tests/injection.h):
 *    every visit must satisfy payload + header + unused == page_size,
 *    every page is visited exactly once, the sum of the visited spans plus
 *    the three meta pages equals first_unallocated, and the five planted
 *    overflow chains span exactly 2+3+4+5+6 pages;
 *
 *  - shape_accounting_exact observes the same database through the public
 *    mdbx_env_chk() surface: the chk engine asserts the same decomposition
 *    (any violation surfaces as a "misused" problem) and aggregates the
 *    per-table page/byte statistics via the table_conclude callback. */

namespace {

const char *kDb = "walk-contracts.mdbx";

/* big-endian 32-bit: the byte-string order then equals the integer order
 * for any value range (portable, no compiler builtins) */
static uint32_t be32(unsigned v) {
  return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

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
  size_t payload_bytes, lost_bytes;
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

/* Builds the shape-rich database on the given path (see the file comment);
 * reports the actual page size via the out parameter. */
static void build_shape_db(const char *path, env_ptr &env, uint32_t &ps) {
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env.get(), 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env.get(), path, MDBX_NOSUBDIR, 0644));

  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env.get(), nullptr, &info, sizeof info));
  ps = info.mi_dxb_pagesize;
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
    /* dupfix duplicates: the small bushes stay sub-pages; the values are
     * big-endian so the byte-string order equals the integer order for any
     * range, and the "df-big" bush (ps/8 values = ps/2 bytes of data)
     * grows into a nested dupfix sub-tree whose leaves are whole P_DUPFIX
     * pages */
    const unsigned dupfix_per_key = ps / 32 > 256 ? 256u : ps / 32;
    for (unsigned k = 0; k < 8; ++k) {
      char key[16];
      const int klen = std::snprintf(key, sizeof key, "df-%02u", k);
      MDBX_val kv = {key, size_t(klen)};
      for (unsigned i = 0; i < dupfix_per_key; ++i) {
        uint32_t v = be32(i);
        MDBX_val dv = {&v, sizeof v};
        ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dupfix, &kv, &dv, MDBX_APPENDDUP));
      }
    }
    {
      MDBX_val kv = {const_cast<char *>("df-big"), 6};
      for (unsigned i = 0; i < ps / 8; ++i) {
        uint32_t v = be32(i);
        MDBX_val dv = {&v, sizeof v};
        ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dupfix, &kv, &dv, MDBX_APPENDDUP));
      }
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
}

} // namespace

TEST(walk_contracts, shape_accounting_exact) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  uint32_t ps = 0;
  build_shape_db(kDb, env, ps);
  (void)ps;

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

TEST(walk_contracts, raw_visitor_byte_accounting) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard({kDb});
  env_ptr env;
  uint32_t ps = 0;
  build_shape_db(kDb, env, ps);

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn));

  struct raw_stats {
    size_t visits, pages_summed, large_visits, large_pages, subpage_visits, branch_visits;
    size_t dupfix_visits, nested_visits, named_table_visits, sub_dupfix_visits;
    uint32_t ps;
    unsigned char seen[8 * 1024];
    bool saw_gc, saw_main, bad_sum, bad_span_bytes, dup_page;
  };
  raw_stats st{};
  st.ps = ps;

  auto visitor =
      +[](void *ctx_, const MDBX_txn *, size_t pgno, unsigned number, unsigned deep, const char *table_name,
          size_t table_name_len, int table_nested, size_t page_size, int page_type, uint64_t, int page_errcode,
          size_t nentries, size_t payload_bytes, size_t header_bytes, size_t unused_bytes, size_t) -> int {
    (void)deep;
    (void)nentries;
    raw_stats *s = static_cast<raw_stats *>(ctx_);
    if (page_errcode != MDBX_SUCCESS)
      return 1000 + page_errcode;
    const std::string name(table_name, table_name_len);
    if (name != "GC" && name != "MAIN")
      ++s->named_table_visits;
    if (name == "GC")
      s->saw_gc = true;
    if (name == "MAIN")
      s->saw_main = true;
    if (table_nested)
      ++s->nested_visits;
    ++s->visits;
    if (payload_bytes + header_bytes + unused_bytes != page_size)
      s->bad_sum = true;
    if (number == 0) {
      ++s->subpage_visits;
      if (page_type == MDBX_INJECTION_PAGE_SUB_DUPFIX_LEAF)
        ++s->sub_dupfix_visits;
      return 0;
    }
    if (pgno < sizeof(s->seen) * 8) {
      if (s->seen[pgno / 8] & (1u << (pgno % 8)))
        s->dup_page = true;
      s->seen[pgno / 8] |= (unsigned char)(1u << (pgno % 8));
    }
    s->pages_summed += number;
    if (number > 1) {
      ++s->large_visits;
      s->large_pages += number;
      if (page_type != MDBX_INJECTION_PAGE_LARGE || page_size != number * s->ps)
        s->bad_span_bytes = true;
    } else {
      if (page_type == MDBX_INJECTION_PAGE_BRANCH)
        ++s->branch_visits;
      if (page_type == MDBX_INJECTION_PAGE_DUPFIX_LEAF)
        ++s->dupfix_visits;
    }
    return 0;
  };

  const int rc = mdbx_injection_walk_pages(txn, visitor, &st, 0 /* full checks */);
  const uint64_t fua = mdbx_injection_geo_first_unallocated(txn);
  mdbx_txn_abort(txn);

  ASSERT_EQ(MDBX_SUCCESS, rc);
  EXPECT_FALSE(st.bad_sum) << "LEM-V4-4: payload+header+unused == page_size for every visit";
  EXPECT_FALSE(st.dup_page) << "every page is visited exactly once";
  EXPECT_FALSE(st.bad_span_bytes) << "a multi-page span is one large-page visit of span bytes";
  /* the corollary: the visited spans + the three meta pages == first_unallocated */
  EXPECT_EQ(fua, (uint64_t)st.pages_summed + 3)
      << "the page accounting is exact (spans + NUM_METAS == first_unallocated)";
  EXPECT_EQ(5u, st.large_visits);
  EXPECT_EQ(2u + 3u + 4u + 5u + 6u, st.large_pages);
  EXPECT_GT(st.subpage_visits, 0u);
  EXPECT_GT(st.branch_visits, 0u);
  EXPECT_GT(st.dupfix_visits, 0u) << "whole P_DUPFIX leaves of the nested dupfix sub-tree";
  EXPECT_GT(st.sub_dupfix_visits, 0u) << "dupfix sub-pages inside the small bushes";
  EXPECT_GT(st.nested_visits, 0u);
  EXPECT_GE(st.named_table_visits, 2u);
  EXPECT_TRUE(st.saw_gc || st.saw_main);
}
