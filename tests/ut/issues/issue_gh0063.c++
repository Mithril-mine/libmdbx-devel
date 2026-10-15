#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/63
// The on-disk 16-bit tree `height` drives an out-of-bounds write during
// root-collapse in tree_rebalance(): the cursor page/index stacks have a
// fixed CURSOR_STACK_SIZE (32 on 64-bit), but the loops that shift the stacks
// were bounded by the stored tree->height (0..65535, read from the file), not
// by the real stack depth. meta_validate() checks each page's own layout, not
// tree metadata, so a crafted file whose trees.main.height (or trees.gc.height)
// is large passes MDBX_VALIDATION and makes a subsequent delete walk past the
// 32-slot arrays.
// The test crafts a database by patching the on-disk meta pages (height at
// meta_t offset 90 for the main tree, 42 for the GC tree) and expects the
// reopen with MDBX_VALIDATION to be rejected with MDBX_CORRUPTED.
static uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

static bool create_base_db(const std::string &testdb, bool retire_pages) {
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS &&
            mdbx_env_set_maxdbs(env, 1) == MDBX_SUCCESS &&
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
  if (ok && retire_pages) {
    /* Delete all keys in a separate transaction so the retired pages land in
     * the GC (FreeDB) tree instead of being reused within the same commit. */
    ok = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS &&
         mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi) == MDBX_SUCCESS;
    MDBX_cursor *cur = nullptr;
    MDBX_val key = {}, data = {};
    if (ok)
      ok = mdbx_cursor_open(txn, dbi, &cur) == MDBX_SUCCESS;
    while (ok && mdbx_cursor_get(cur, &key, &data, MDBX_FIRST) == MDBX_SUCCESS) {
      ok = mdbx_cursor_del(cur, MDBX_put_flags_t(0)) == MDBX_SUCCESS;
      if (ok)
        mdbx_cursor_get(cur, &key, &data, MDBX_NEXT);
    }
    if (cur)
      mdbx_cursor_close(cur);
    if (ok)
      ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
    else
      mdbx_txn_abort(txn);
  }
  mdbx_env_close(env);
  return ok;
}

// Patch the given tree's height (meta_t + 90 for the main tree, +42 for the
// GC tree) in every meta page of the datafile.
static bool patch_meta_height(const std::string &testdb, size_t height_off, uint16_t height) {
  constexpr uint32_t pagesize = 4096;
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

  // The meta payload starts at PAGEHDRSZ within the page; the page size is
  // stored in the trees.gc.dupfix_size/pagesize union field at meta_t + 44.
  constexpr size_t PAGEHDRSZ = 20;
  const uint32_t file_pagesize = read_u32_le(&buf[PAGEHDRSZ + 44]);
  if (file_pagesize != pagesize)
    return false;

  bool patched = false;
  for (size_t off = 0; off + pagesize <= buf.size(); off += pagesize) {
    const uint16_t flags = read_u16_le(&buf[off + 10]);
    if (flags == 0x08 /* P_META */) {
      write_u16_le(&buf[off + height_off], height);
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

static int reopen_with_validation(const std::string &testdb) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_env_set_maxdbs(env, 1);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return rc;
  }
  rc = mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR | MDBX_VALIDATION, 0);
  mdbx_env_close(env);
  return rc;
}

TEST(issue_gh0063, main_tree_height_rejected) {
  const std::string testdb = "issue63main.mdbx";
  ASSERT_TRUE(create_base_db(testdb, /* retire_pages = */ false));
  // trees.main.height is at meta_t + 90; the GC tree of this database is
  // empty (root == P_INVALID, height == 0), so the patch only corrupts the
  // main tree record.
  ASSERT_TRUE(patch_meta_height(testdb, 20 + 90, 0xFFFF));
  EXPECT_EQ(MDBX_CORRUPTED, reopen_with_validation(testdb));
  mdbx::env::remove(testdb);
}

TEST(issue_gh0063, gc_tree_height_rejected) {
  const std::string testdb = "issue63gc.mdbx";
  // Deleting all keys retires pages, so the GC (FreeDB) tree gets a valid
  // root and a real height; patching trees.gc.height (meta_t + 42) then
  // cannot be rejected by the false-empty rule and exercises the new bound.
  ASSERT_TRUE(create_base_db(testdb, /* retire_pages = */ true));
  ASSERT_TRUE(patch_meta_height(testdb, 20 + 42, 0xFFFF));
  EXPECT_EQ(MDBX_CORRUPTED, reopen_with_validation(testdb));
  mdbx::env::remove(testdb);
}
