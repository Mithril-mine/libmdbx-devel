#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

/* Regression for https://github.com/Mithril-mine/libmdbx-devel/issues/454:
 * a crafted DUPSORT database whose nested dupset tree carries an on-disk
 * height beyond the cursor stack made cursor_seek() return MDBX_CORRUPTED
 * from cursor_dupsort_setup() while leaving the cursor positioned, which
 * violates cursor_ops()'s postcondition asserted as
 * `is_poor(mc) && !is_filled(mc)` — so a crafted file aborted the
 * assert-enabled builds instead of returning MDBX_CORRUPTED (release builds
 * were unaffected). Now the seek path leaves the cursor poor on the
 * dupsort_setup() failure, like unexpected_dupsort() does.
 *
 * NOTE: this is a format-dependent test. It inspects the on-disk layout
 * directly (page header offsets, node flags N_DUP|N_TREE and the tree_t
 * height field), so it must be revisited together with any on-disk format
 * change. */

namespace {

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

uint16_t read_u16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

/* Creates the database: one DUPSORT key "K" with a page-size-scaled number
 * of duplicates, so the dupset is stored as a nested sub-DB (N_TREE). */
void create_db(const char *path, unsigned dup_count) {
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), path, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_CREATE, &dbi), MDBX_SUCCESS);
  MDBX_val key = {const_cast<void *>(static_cast<const void *>("K")), 1};
  for (unsigned j = 0; j < dup_count; ++j) {
    MDBX_cursor *mc = nullptr;
    ASSERT_EQ(mdbx_cursor_open(txn, dbi, &mc), MDBX_SUCCESS);
    char vb[16];
    MDBX_val data = {vb, (size_t)std::snprintf(vb, sizeof vb, "v-%06u", j)};
    const int rc = mdbx_cursor_put(mc, &key, &data, MDBX_APPENDDUP);
    EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_KEYEXIST);
    mdbx_cursor_close(mc);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

/* Patches the dupset node's tree_t height to 255 (beyond CURSOR_STACK_SIZE).
 * Returns true when the node was found and patched. */
bool craft_height(const char *path, uint32_t pagesize) {
  FILE *f = std::fopen(path, "r+b");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)size);
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  bool patched = false;
  const size_t ps = pagesize;
  for (size_t off = 2 * ps; off + ps <= buf.size() && !patched; off += ps) {
    const uint16_t flags = read_u16(&buf[off + 10]);
    if ((flags & 0x06) != 0x02) /* leaf-ish pages only */
      continue;
    const uint16_t lower = read_u16(&buf[off + 12]);
    const size_t nkeys = lower / 2;
    for (size_t i = 0; i < nkeys && !patched; ++i) {
      const uint16_t entry = read_u16(&buf[off + 20 + 2 * i]);
      uint8_t *node = buf.data() + off + entry + 20;
      if (node[4] == 0x06 /* N_DUP | N_TREE */ && node[6] == 1 /* ksize of "K" */ &&
          std::memcmp(node + 8, "K", 1) == 0) {
        uint8_t *tree = node + 8 + 1; /* the payload: key then tree_t */
        tree[2] = 0xFF;               /* height = 255 */
        tree[3] = 0x00;
        std::fseek(f, (long)off, SEEK_SET);
        if (std::fwrite(buf.data() + off, 1, ps, f) != ps)
          return false;
        patched = true;
      }
    }
  }
  std::fclose(f);
  return patched;
}

} // namespace

TEST(issue_gh0454, crafted_dupset_height_returns_corrupted) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const kDb = "issue454.mdbx";
  const env_files_guard files({kDb});

  /* 3000 duplicates exceed any page size, so the dupset is stored as a
   * nested sub-DB (N_TREE) regardless of the platform's page size */
  create_db(kDb, 3000);

  /* probe the actual page size from the created database */
  uint32_t pagesize = 0;
  {
    env_ptr probe;
    ASSERT_EQ(mdbx_env_create(&probe), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_env_open(probe.get(), kDb, MDBX_NOSUBDIR | MDBX_RDONLY, 0644), MDBX_SUCCESS);
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof info);
    ASSERT_EQ(mdbx_env_info_ex(probe.get(), nullptr, &info, sizeof info), MDBX_SUCCESS);
    pagesize = info.mi_dxb_pagesize;
    ASSERT_GE(pagesize, 256u);
  }
  ASSERT_TRUE(craft_height(kDb, pagesize)) << "the nested dupset tree node must be found and patched";

  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  MDBX_cursor *mc = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &mc), MDBX_SUCCESS);
  MDBX_val key = {const_cast<void *>(static_cast<const void *>("K")), 1}, data = {nullptr, 0};
  const int rc = mdbx_cursor_get(mc, &key, &data, MDBX_SET);
  mdbx_cursor_close(mc);
  mdbx_txn_abort(txn);
  /* the env is closed exactly once, by the env_ptr destructor: an explicit
   * close here would leave the destructor closing the freed handle again
   * (use-after-free under ASan, silent UB otherwise) */

  ASSERT_EQ(MDBX_CORRUPTED, rc)
      << "a crafted dupset height must be rejected with MDBX_CORRUPTED, not abort the process";
}
