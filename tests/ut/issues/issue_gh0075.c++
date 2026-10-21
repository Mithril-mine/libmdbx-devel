#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/75
// The range estimator divided by tree->leaf_pages / tree->branch_pages taken
// straight from the on-disk tree record, with no zero check. A crafted file
// with a valid root but a zero page count made mdbx_estimate_*() trap with a
// hardware divide-by-zero (SIGFPE).
//
// meta_validate() must reject such a tree record. Its height counts every level
// from the root down to the leaf pages (nested sub-tables and overflow/large
// pages are not counted), so all those levels are leaf or branch pages, and the
// following equivalences must hold:
//   (height == 0) == (root == P_INVALID)
//   (height == 0) == (leaf_pages == 0)
//   (height >  1) == (branch_pages > 0)

static uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
static void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

/* tree_t field offsets within tree_t (see src/layout-dxb.h) */
static constexpr size_t tree_height_off = 2;
static constexpr size_t tree_branch_pages_off = 12;
static constexpr size_t tree_leaf_pages_off = 16;

static bool create_base_db(const std::string &testdb, size_t item_count) {
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 1) == MDBX_SUCCESS &&
            mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, pagesize) == MDBX_SUCCESS &&
            mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS &&
            mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS &&
            mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi) == MDBX_SUCCESS;
  if (!ok) {
    if (env)
      mdbx_env_close(env);
    return false;
  }
  char keybuf[32], databuf[64];
  for (size_t i = 0; i < item_count && ok; i++) {
    std::snprintf(keybuf, sizeof keybuf, "bulk-%03zu", i);
    std::memset(databuf, 'x', sizeof databuf);
    MDBX_val key = {keybuf, std::strlen(keybuf)};
    MDBX_val data = {databuf, sizeof databuf};
    ok = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) == MDBX_SUCCESS;
  }
  if (ok)
    ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
  else
    mdbx_txn_abort(txn);
  mdbx_env_close(env);
  return ok;
}

/* Overwrite a field of trees.main (which starts at meta_t + 88, i.e. at
 * PAGEHDRSZ + 88 within the page) in every meta page of the datafile, keeping
 * the rest of the tree record untouched. */
template <typename Writer>
static bool patch_main_field(const std::string &testdb, size_t field_off_in_tree, Writer writer) {
  constexpr uint32_t pagesize = 4096;
  constexpr size_t PAGEHDRSZ = 20;
  const size_t field_off = PAGEHDRSZ + 88 + field_off_in_tree;

  std::fstream f(testdb, std::ios::in | std::ios::out | std::ios::binary);
  if (!f.is_open())
    return false;
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  if (end <= 0)
    return false;
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  if (f.gcount() != std::streamsize(buf.size()))
    return false;

  const uint32_t file_pagesize = read_u32_le(&buf[PAGEHDRSZ + 44]);
  if (file_pagesize != pagesize)
    return false;

  bool patched = false;
  for (size_t off = 0; off + pagesize <= buf.size(); off += pagesize) {
    const uint16_t flags = read_u16_le(&buf[off + 10]);
    if (flags == 0x08 /* P_META */) {
      writer(&buf[off + field_off]);
      patched = true;
    }
  }
  if (!patched)
    return false;

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return true;
}

static void open_expect_corrupted(const std::string &testdb, const char *what) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 1));
  EXPECT_EQ(MDBX_CORRUPTED, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR | MDBX_VALIDATION, 0)) << what;
  mdbx_env_close(env);
}

/* 200 64-byte items produce a two-level tree: height == 2, branch_pages == 1,
 * leaf_pages == 5. */
static constexpr size_t tall_item_count = 200;
/* 5 64-byte items fit in a single leaf page: height == 1, branch_pages == 0. */
static constexpr size_t shallow_item_count = 5;

TEST(issue_gh0075, zero_leaf_pages_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue75-leaf.mdbx";
  ASSERT_TRUE(create_base_db(testdb, tall_item_count));
  ASSERT_TRUE(patch_main_field(testdb, tree_leaf_pages_off, [](uint8_t *p) { write_u32_le(p, 0); }));
  open_expect_corrupted(testdb, "a non-empty tree with zero leaf pages must be rejected");
  mdbx::env::remove(testdb);
}

TEST(issue_gh0075, zero_height_with_valid_root_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue75-height.mdbx";
  ASSERT_TRUE(create_base_db(testdb, tall_item_count));
  ASSERT_TRUE(patch_main_field(testdb, tree_height_off, [](uint8_t *p) { write_u16_le(p, 0); }));
  open_expect_corrupted(testdb, "a valid root with a zero height must be rejected");
  mdbx::env::remove(testdb);
}

TEST(issue_gh0075, zero_branch_pages_tall_tree_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue75-branch.mdbx";
  ASSERT_TRUE(create_base_db(testdb, tall_item_count));
  ASSERT_TRUE(patch_main_field(testdb, tree_branch_pages_off, [](uint8_t *p) { write_u32_le(p, 0); }));
  open_expect_corrupted(testdb, "a tree taller than a single leaf page with zero branch pages must be rejected");
  mdbx::env::remove(testdb);
}

TEST(issue_gh0075, spurious_branch_pages_shallow_tree_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue75-branchspur.mdbx";
  ASSERT_TRUE(create_base_db(testdb, shallow_item_count));
  ASSERT_TRUE(patch_main_field(testdb, tree_branch_pages_off, [](uint8_t *p) { write_u32_le(p, 1); }));
  open_expect_corrupted(testdb, "a single-leaf tree with branch pages must be rejected");
  mdbx::env::remove(testdb);
}
