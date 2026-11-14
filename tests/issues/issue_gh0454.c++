/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/454:
 * a crafted DUPSORT database whose nested dupset tree carries an on-disk
 * height beyond the cursor stack made cursor_seek() return MDBX_CORRUPTED
 * from cursor_dupsort_setup() while leaving the cursor positioned, which
 * violates cursor_ops()'s postcondition asserted as
 * `is_poor(mc) && !is_filled(mc)` - so a crafted file aborted the
 * assert-enabled builds instead of returning MDBX_CORRUPTED (release builds
 * were unaffected). Now the seek path leaves the cursor poor on the
 * dupsort_setup() failure, like unexpected_dupsort() does.
 *
 * NOTE: this is a format-dependent test. It inspects the on-disk layout
 * directly (page header offsets, node flags N_DUP|N_TREE and the tree_t
 * height field), so it must be revisited together with any on-disk format
 * change. */
/* the file-access helpers below use the classic CRT file API,
 * which MSVC deprecates unless explicitly allowed */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif /* _CRT_SECURE_NO_WARNINGS */
#include "mdbx.h++"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint16_t read_u16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

static void create_db(const char *path, unsigned dup_count) {
  int rc;
  MDBX_env *env = nullptr;
  rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return;
  mdbx_env_set_maxdbs(env, 4);
  rc = mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return;
  }
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_CREATE, &dbi);
  char vb[16];
  for (unsigned j = 0; rc == MDBX_SUCCESS && j < dup_count; ++j) {
    MDBX_cursor *mc = nullptr;
    rc = mdbx_cursor_open(txn, dbi, &mc);
    if (rc != MDBX_SUCCESS)
      break;
    MDBX_val key = {const_cast<void *>(static_cast<const void *>("K")), 1};
    MDBX_val data = {vb, (size_t)std::snprintf(vb, sizeof vb, "v-%06u", j)};
    rc = mdbx_cursor_put(mc, &key, &data, MDBX_APPENDDUP);
    mdbx_cursor_close(mc);
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

static bool craft_height(const char *path, uint32_t pagesize) {
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

int main() {
  const char *const db = "issue454.mdbx";
  mdbx::env::remove(db);

  /* 3000 duplicates exceed any page size, so the dupset is stored as a
   * nested sub-DB (N_TREE) regardless of the platform's page size */
  create_db(db, 3000);

  uint32_t pagesize = 0;
  {
    MDBX_env *probe = nullptr;
    int rc = mdbx_env_create(&probe);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_env_open(probe, db, MDBX_NOSUBDIR | MDBX_RDONLY, 0644);
    if (rc == MDBX_SUCCESS) {
      MDBX_envinfo info;
      std::memset(&info, 0, sizeof info);
      rc = mdbx_env_info_ex(probe, nullptr, &info, sizeof info);
      if (rc == MDBX_SUCCESS)
        pagesize = info.mi_dxb_pagesize;
    }
    if (probe)
      mdbx_env_close(probe);
    if (rc != MDBX_SUCCESS || pagesize < 256) {
      std::fprintf(stderr, "FAIL: cannot probe the page size\n");
      return EXIT_FAILURE;
    }
  }
  if (!craft_height(db, pagesize)) {
    std::fprintf(stderr, "FAIL: the nested dupset tree node was not found\n");
    mdbx::env::remove(db);
    return EXIT_FAILURE;
  }

  int rc;
  MDBX_env *env = nullptr;
  rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_set_maxdbs(env, 4);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, db, MDBX_NOSUBDIR, 0644);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "t", MDBX_DUPSORT, &dbi);
  MDBX_cursor *mc = nullptr;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_cursor_open(txn, dbi, &mc);
  MDBX_val key = {const_cast<void *>(static_cast<const void *>("K")), 1}, data = {nullptr, 0};
  if (rc == MDBX_SUCCESS)
    rc = mdbx_cursor_get(mc, &key, &data, MDBX_SET);
  if (mc)
    mdbx_cursor_close(mc);
  if (txn)
    mdbx_txn_abort(txn);
  if (env)
    mdbx_env_close(env);
  mdbx::env::remove(db);

  if (rc != MDBX_CORRUPTED) {
    std::fprintf(stderr, "FAIL: rc %d instead of MDBX_CORRUPTED\n", rc);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
