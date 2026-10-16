#include "mdbx.h++"
#include "tool_runner.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/76
// mdbx_chk reads out of bounds on a crafted GC record whose leading page-count
// word pnl[0] exceeds PAGELIST_LIMIT: chk_handle_gc() reported "wrong idl
// length" but did not clamp the count (the size clamp lived in an else-if), so
// the accounting and the per-entry walk indexed far past the record. The tool
// meant to diagnose damaged files crashed on it. The over-limit count must be
// clamped to what the record can actually hold, so mdbx_chk completes (with a
// non-zero exit) instead of crashing.
// The test crafts a database by patching the first page-count word of a GC
// record to 0xFFFFFFFF, then runs mdbx_chk and expects it to finish normally
// with an error, not to die by a signal.

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

static bool create_gc_db(const std::string &testdb) {
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  /* The test crafts a 4096-byte-page file and hardcodes that page size while
   * locating pages, so pin the geometry to 4096-byte pages instead of relying
   * on the (platform-dependent) system page size. */
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 8) == MDBX_SUCCESS &&
            mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, 4096) == MDBX_SUCCESS &&
            mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
  if (!ok) {
    if (env)
      mdbx_env_close(env);
    return false;
  }
  /* Insert in one write transaction, delete in a separate one, so that the
   * discarded pages are tracked by the GC tree. */
  ok = mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) == MDBX_SUCCESS &&
       mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi) == MDBX_SUCCESS;
  for (int i = 0; ok && i < 20000; ++i) {
    char kb[24], vb[24];
    const int kn = snprintf(kb, sizeof kb, "key%08d", i);
    const int vn = snprintf(vb, sizeof vb, "val%08d", i);
    MDBX_val key = {kb, (size_t)kn};
    MDBX_val val = {vb, (size_t)vn};
    ok = mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT) == MDBX_SUCCESS;
  }
  if (ok)
    ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
  else
    mdbx_txn_abort(txn);

  if (ok) {
    ok = mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) == MDBX_SUCCESS;
    MDBX_cursor *mc = nullptr;
    MDBX_val key = {}, data = {};
    if (ok)
      ok = mdbx_cursor_open(txn, dbi, &mc) == MDBX_SUCCESS;
    while (ok && mdbx_cursor_get(mc, &key, &data, MDBX_NEXT) == MDBX_SUCCESS)
      ok = mdbx_cursor_del(mc, MDBX_put_flags_t(0)) == MDBX_SUCCESS;
    if (mc)
      mdbx_cursor_close(mc);
    if (ok)
      ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
    else
      mdbx_txn_abort(txn);
  }
  mdbx_env_close(env);
  return ok;
}

static bool patch_first_gc_record(std::vector<uint8_t> &buf, size_t pagesize, uint32_t pgno, bool &patched) {
  const size_t off = (size_t)pgno * pagesize;
  if (off + pagesize > buf.size())
    return false;
  const uint16_t flags = read_u16_le(&buf[off + 10]);
  const uint16_t lower = read_u16_le(&buf[off + 12]);
  const size_t nkeys = lower / 2;
  for (size_t i = 0; i < nkeys; ++i) {
    const uint16_t entry = read_u16_le(&buf[off + 20 + 2 * i]);
    const size_t node_off = off + entry + 20; /* node_t starts here */
    if (flags & 0x01 /* P_BRANCH */) {
      const uint32_t child = read_u32_le(&buf[node_off]); /* node->dsize == child_pgno */
      if (!patch_first_gc_record(buf, pagesize, child, patched))
        return false;
      if (patched)
        return true;
    } else if (flags & 0x02 /* P_LEAF */) {
      const uint16_t ksize = read_u16_le(&buf[node_off + 6]);
      const uint32_t dsize = read_u32_le(&buf[node_off]);
      /* A GC record: an 8-byte txnid key and a page list of at least one
       * entry; patch the leading count word to exceed PAGELIST_LIMIT. */
      if (ksize == 8 && dsize >= 16 && node_off + 8 + 8 + sizeof(uint32_t) <= off + pagesize) {
        write_u32_le(&buf[node_off + 8 + 8], 0xFFFFFFFFu); /* pnl[0] = count */
        patched = true;
        return true;
      }
    }
  }
  return true;
}

TEST(issue_gh0076, chk_survives_over_limit_gc_count) {
  const std::string testdb = "issue76.mdbx";
  ASSERT_TRUE(create_gc_db(testdb));

  std::fstream f(testdb, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(f.is_open());
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  ASSERT_GT(end, 0);
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  ASSERT_EQ(f.gcount(), std::streamsize(buf.size()));

  constexpr size_t PAGEHDRSZ = 20;
  /* find the valid meta page: its GC tree must have a real (non-P_INVALID)
   * root; the older/other meta slots may still be empty */
  uint32_t pagesize = 0, gc_root = 0xFFFFFFFFu;
  for (uint32_t pg = 0; pg < 3 && gc_root == 0xFFFFFFFFu; ++pg) {
    const size_t off = (size_t)pg * 4096;
    if (off + 4096 > buf.size())
      break;
    const uint16_t flags = read_u16_le(&buf[off + 10]);
    if (flags != 0x08 /* P_META */)
      continue;
    pagesize = read_u32_le(&buf[off + PAGEHDRSZ + 44]);
    gc_root = read_u32_le(&buf[off + PAGEHDRSZ + 48]); /* meta_t.gc.root */
  }
  ASSERT_EQ(4096u, pagesize);
  ASSERT_NE(gc_root, 0xFFFFFFFFu) << "the GC tree must have a valid root";

  bool patched = false;
  ASSERT_TRUE(patch_first_gc_record(buf, pagesize, gc_root, patched));
  ASSERT_TRUE(patched) << "no GC record found to patch";

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  ASSERT_TRUE(f.good());
  f.close();

  /* Run mdbx_chk on the crafted file: it must detect the wrong idl length and
   * exit with a small non-zero status, not die by a signal (crash). */
  const std::string chk_tool = tool_runner::tool_path("mdbx_chk");
  const int rc = tool_runner::run({chk_tool, testdb});
  EXPECT_GT(rc, 0) << "mdbx_chk must report the crafted GC record (rc=" << rc << ")";
  EXPECT_LT(rc, 128) << "mdbx_chk must not crash on the crafted GC record (rc=" << rc << ")";

  mdbx::env::remove(testdb);
}
