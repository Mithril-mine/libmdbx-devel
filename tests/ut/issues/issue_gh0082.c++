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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/82
// walk_pgno()/walk_pages() have no visited-set: a depth-first traversal trusts
// each branch node's child page number and descends into it, so a crafted tree
// whose branch pages reference a shared sub-tree (a reconverging DAG) is walked
// once per root-to-leaf path, doing ~2^depth of work and growing arcs without
// bound; the only guard bounds the depth of a single path, so paths of length
// under 32 are fine while their exponential number is not. The walk now rejects
// any page referenced more than once (a valid copy-on-write tree never shares a
// page) with MDBX_CORRUPTED, and bounds the total number of walked pages.
// The test crafts a two-level main tree whose root branch page lists the same
// leaf twice and expects mdbx_env_defrag() to fail with MDBX_CORRUPTED.

static uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

static bool create_base_db(const std::string &testdb) {
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 1) == MDBX_SUCCESS &&
            mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, pagesize) == MDBX_SUCCESS &&
            mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS &&
            mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS &&
            mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) == MDBX_SUCCESS;
  if (!ok) {
    if (env)
      mdbx_env_close(env);
    return false;
  }
  /* 20000 64-byte items (half of them deleted afterwards to provide the GC
   * stockpile that makes defragmentation run) produce a multi-level main tree
   * whose root is a branch page with child pointers into branch/leaf pages. */
  char keybuf[32], databuf[64];
  for (int i = 0; i < 20000 && ok; i++) {
    std::snprintf(keybuf, sizeof keybuf, "bulk-%04d", i);
    std::memset(databuf, 'x', sizeof databuf);
    MDBX_val key = {keybuf, std::strlen(keybuf)};
    MDBX_val data = {databuf, sizeof databuf};
    ok = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) == MDBX_SUCCESS;
  }
  for (int i = 10000; i < 20000 && ok; i++) {
    std::snprintf(keybuf, sizeof keybuf, "bulk-%04d", i);
    MDBX_val key = {keybuf, std::strlen(keybuf)};
    ok = mdbx_del(txn, dbi, &key, nullptr) == MDBX_SUCCESS;
  }
  if (ok)
    ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
  else
    mdbx_txn_abort(txn);
  mdbx_env_close(env);
  return ok;
}

/* Make the main tree's root branch page reference the same leaf twice (the
 * first leaf is both the first and the second child): a valid tree never has
 * two parent slots pointing to one page. */
static bool craft_shared_leaf(const std::string &testdb) {
  constexpr uint32_t pagesize = 4096;
  constexpr size_t PAGEHDRSZ = 20;
  const size_t main_root_off = PAGEHDRSZ + 96; /* meta_t + 88 + tree_t.root */
  const size_t meta_pagesize_off = PAGEHDRSZ + 44;

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

  if (read_u32_le(&buf[meta_pagesize_off]) != pagesize)
    return false;

  uint32_t root = 0;
  bool found = false;
  for (size_t off = 0; off + pagesize <= buf.size(); off += pagesize) {
    if (read_u16_le(&buf[off + 10]) != 0x08 /* P_META */)
      continue;
    root = read_u32_le(&buf[off + main_root_off]);
    if (root != 0xFFFFFFFFu && root >= 3 /* NUM_METAS */) {
      found = true;
      break;
    }
  }
  if (!found)
    return false;

  const size_t poff = (size_t)root * pagesize;
  if (poff + pagesize > buf.size())
    return false;
  const uint16_t flags = read_u16_le(&buf[poff + 10]);
  if ((flags & 0x01 /* P_BRANCH */) == 0)
    return false;
  const uint16_t lower = read_u16_le(&buf[poff + 12]);
  const size_t nkeys = (size_t)lower / 2;
  if (nkeys < 2)
    return false;

  const uint16_t entry0 = read_u16_le(&buf[poff + PAGEHDRSZ]);
  const uint16_t entry1 = read_u16_le(&buf[poff + PAGEHDRSZ + 2]);
  if (entry0 == entry1)
    return false;
  const size_t node0_off = poff + PAGEHDRSZ + entry0;
  const size_t node1_off = poff + PAGEHDRSZ + entry1;
  if (node1_off + sizeof(uint32_t) > poff + pagesize)
    return false;
  /* node_t.child_pgno is the first field of a branch node: make the second
   * child point to the same leaf as the first one (shared sub-tree). */
  write_u32_le(&buf[node1_off], read_u32_le(&buf[node0_off]));

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return true;
}

TEST(issue_gh0082, defrag_rejects_shared_page) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue82.mdbx";
  ASSERT_TRUE(create_base_db(testdb));
  ASSERT_TRUE(craft_shared_leaf(testdb)) << "must be able to craft the shared-leaf tree";

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));

  /* Defragmentation loads the b-tree structure with walk_pages() (it needs some
   * free pages, provided by the deleted half above): a page referenced from two
   * parent slots must be treated as corruption, not walked twice. */
  MDBX_defrag_result_t result = {};
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, 0, 4 << 20, nullptr, nullptr, &result);
  EXPECT_EQ(MDBX_CORRUPTED, rc) << "a tree with a shared page must be rejected, not silently de-duplicated";
  EXPECT_NE(0u, result.stopping_reasons & MDBX_defrag_error);

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
}