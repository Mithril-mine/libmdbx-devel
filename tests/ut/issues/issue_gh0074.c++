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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/74
// tree_cutoff_twig() walks a tree to retire its pages when a table is dropped
// (or cleared) and recurses into every child of every branch page, bounding the
// recursion only by the stored tree height in a fast path that is DISABLED
// under MDBX_VALIDATION. MDBX_VALIDATION's per-page checks validate each page's
// own layout but keep no visited-set, so a crafted file whose branch page
// points a child back at itself (a cycle) makes the recursion grow without
// bound until the C stack overflows.
// The test crafts such a file (a two-level main tree whose root branch page
// points its first child back at itself), opens it with MDBX_VALIDATION and
// drops the table: the drop must be rejected with MDBX_CORRUPTED, not crash.

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
  /* 200 64-byte items produce a two-level main tree (height == 2), whose root
   * is a branch page with child pointers into leaf pages. */
  char keybuf[32], databuf[64];
  for (int i = 0; i < 200 && ok; i++) {
    std::snprintf(keybuf, sizeof keybuf, "bulk-%03d", i);
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

/* Make the main tree's root branch page point its first child back at the root
 * itself, forming a cycle that tree_cutoff_twig() would follow forever. */
static bool craft_cyclic_root(const std::string &testdb) {
  constexpr uint32_t pagesize = 4096;
  constexpr size_t PAGEHDRSZ = 20;
  /* trees.main.root is at meta_t + 96 (main tree at meta_t + 88, root at
   * tree_t + 8), and the meta payload starts at PAGEHDRSZ within the page. */
  const size_t main_root_off = PAGEHDRSZ + 96;

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

  /* Find a meta whose main tree has a real root (a fresh database has several
   * meta slots; only the populated one has root != P_INVALID). */
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
  const size_t nkeys = lower / 2;
  if (nkeys == 0)
    return false;
  const uint16_t entry = read_u16_le(&buf[poff + PAGEHDRSZ]);
  const size_t node_off = poff + PAGEHDRSZ + entry;
  if (node_off + sizeof(uint32_t) > poff + pagesize)
    return false;
  /* node_t.child_pgno (the first field of the branch node) := root */
  write_u32_le(&buf[node_off], root);

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return true;
}

TEST(issue_gh0074, cyclic_branch_rejected_on_drop) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue74.mdbx";
  ASSERT_TRUE(create_base_db(testdb));
  ASSERT_TRUE(craft_cyclic_root(testdb));

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR | MDBX_VALIDATION, 0))
      << "the crafted cyclic tree must still open under MDBX_VALIDATION";

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  EXPECT_EQ(MDBX_CORRUPTED, mdbx_drop(txn, dbi, false))
      << "dropping a table with a cyclic branch page must be rejected, not crash";
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  mdbx::env::remove(testdb);
}
