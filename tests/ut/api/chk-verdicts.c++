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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

/* mdbx_env_chk() verdicts on crafted databases: every planted corruption
 * must surface as a specific counted problem (the `issue` callback caption)
 * and turn the return code into MDBX_PROBLEM, per the chk contract
 * (MDBX_PROBLEM + total_problems is a result, not a failure):
 *
 *  - a page number listed in two GC records (issues #83/#452 family):
 *    "already listed in GC", counted into problems_gc;
 *  - a named-table record whose root points beyond the allocated space
 *    (the #454/geometry family): "wrong page-no";
 *  - a named-table record with a height disagreeing with the actual tree:
 *    a "wrong ... tree height" verdict.
 *
 * The positive control is a clean database: zero problems, MDBX_SUCCESS.
 *
 * NOTE: format-dependent. The crafting inspects the on-disk layout directly
 * (page header offsets, node flags N_TREE, the tree_t serialization inside
 * MAIN records and the PNL serialization in GC records), so it must be
 * revisited together with any on-disk format change. */

namespace {

constexpr uint32_t kPBranch = 0x01, kPLeaf = 0x02, kPMeta = 0x08;
constexpr uint32_t kNTree = 0x02;
constexpr size_t kPageHdrSz = 20;

uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

/*---------------------------------------------------------------------------*/

/* issue-caption collector wired through the (otherwise zeroed) callbacks */
struct issue_log {
  std::vector<std::string> captions;
  static void on_issue(MDBX_chk_context_t *, const char *, uint64_t, const char *issue, const char *, va_list) {
    /* the captions are static per caption kind, but copy them anyway */
    instance()->captions.emplace_back(issue);
  }
  static issue_log *instance() { return s_current; }
  static issue_log *s_current;
};
issue_log *issue_log::s_current = nullptr;

/*---------------------------------------------------------------------------*/

/* Build a database with several GC records: each write commit retires the
 * previous revision's pages; a lingering reader in a separate thread pins
 * the first snapshot so retired pages stay listed instead of being recycled
 * and every subsequent commit leaves its own GC record behind. */
struct reader_ctx {
  MDBX_env *env;
  std::atomic<bool> started, stop;
};

void reader_thread(reader_ctx *ctx) {
  MDBX_txn *txn = nullptr;
  mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &txn);
  ctx->started.store(true, std::memory_order_release);
  while (!ctx->stop.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (txn)
    mdbx_txn_abort(txn);
}

bool create_gc_db(const std::string &path) {
  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 2) == MDBX_SUCCESS &&
            mdbx_env_open(env, path.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
  if (!ok) {
    if (env)
      mdbx_env_close(env);
    return false;
  }
  MDBX_txn *txn = nullptr;
  ok = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS;
  if (ok)
    ok = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) == MDBX_SUCCESS;
  for (int i = 0; ok && i < 50; ++i) {
    char key[16], val[128];
    std::snprintf(key, sizeof key, "k%02d", i);
    std::memset(val, 'a' + (char)(i % 26), sizeof val);
    MDBX_val k = {key, std::strlen(key)}, v = {val, sizeof val};
    ok = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) == MDBX_SUCCESS;
  }
  if (ok)
    ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
  else
    mdbx_txn_abort(txn);

  reader_ctx reader = {env, {false}, {false}};
  std::thread thr(reader_thread, &reader);
  while (!reader.started.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (unsigned round = 0; ok && round < 3; ++round) {
    txn = nullptr;
    ok = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS;
    for (int i = 0; ok && i < 15; ++i) {
      char key[16], val[128];
      std::snprintf(key, sizeof key, "k%u_%u", round, i);
      std::memset(val, 'x' + (char)round, sizeof val);
      MDBX_val k = {key, std::strlen(key)}, v = {val, sizeof val};
      ok = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) == MDBX_SUCCESS;
    }
    if (ok)
      ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
    else
      mdbx_txn_abort(txn);
  }
  reader.stop.store(true, std::memory_order_release);
  thr.join();
  mdbx_env_close(env);
  return ok;
}

struct gc_record {
  size_t data_off, count, last_pgno_off;
};

bool walk_gc_leaf(const std::vector<uint8_t> &buf, uint32_t pagesize, uint32_t pgno, size_t depth,
                  std::vector<gc_record> &out) {
  if (depth > 32)
    return false;
  const size_t off = (size_t)pgno * pagesize;
  if (off + pagesize > buf.size())
    return false;
  const uint16_t flags = read_u16_le(&buf[off + 10]);
  const uint16_t lower = read_u16_le(&buf[off + 12]);
  const size_t nkeys = lower / 2;
  for (size_t i = 0; i < nkeys; ++i) {
    const uint16_t entry = read_u16_le(&buf[off + kPageHdrSz + 2 * i]);
    const size_t node_off = off + entry + kPageHdrSz;
    if (flags & kPBranch) {
      const uint32_t child = read_u32_le(&buf[node_off]);
      if (!walk_gc_leaf(buf, pagesize, child, depth + 1, out) || out.size() >= 2)
        return true;
    } else if (flags & kPLeaf) {
      const uint16_t ksize = read_u16_le(&buf[node_off + 6]);
      const uint32_t dsize = read_u32_le(&buf[node_off]);
      const size_t data_off = node_off + 8 + ksize;
      if (ksize == 8 /* txnid key */ && dsize >= sizeof(uint32_t) && data_off + dsize <= off + pagesize) {
        gc_record rec;
        rec.data_off = data_off;
        rec.count = read_u32_le(&buf[data_off]);
        if (rec.count > 0 && rec.count <= 0x1FFFFFu &&
            data_off + (rec.count + 1) * sizeof(uint32_t) <= off + pagesize) {
          rec.last_pgno_off = data_off + rec.count * sizeof(uint32_t);
          out.push_back(rec);
        }
      }
    }
  }
  return out.size() < 2;
}

/* Make two GC records share one page (the lists are stored in descending
 * order, so a record's last element is its least page): store the donor's
 * least page over the victim's least page. Returns 1 crafted, 0 when the GC
 * holds fewer than two records (coalescing merged them - not applicable),
 * -1 on failure. */
int craft_shared_gc_page(const std::string &path, uint32_t pagesize) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f.is_open())
    return -1;
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  if (end <= 0)
    return -1;
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  if (f.gcount() != std::streamsize(buf.size()))
    return -1;

  uint32_t root = 0xFFFFFFFFu;
  for (uint32_t pg = 0; pg < 3; ++pg) {
    const size_t off = (size_t)pg * pagesize;
    if (off + pagesize > buf.size())
      break;
    if (read_u16_le(&buf[off + 10]) != kPMeta)
      continue;
    if (read_u32_le(&buf[off + kPageHdrSz + 44]) != pagesize)
      continue;
    root = read_u32_le(&buf[off + kPageHdrSz + 48]);
    if (root != 0xFFFFFFFFu && root >= 3)
      break;
  }
  if (root == 0xFFFFFFFFu)
    return -1;

  std::vector<gc_record> recs;
  walk_gc_leaf(buf, pagesize, root, 0, recs);
  if (recs.size() < 2)
    return 0;
  uint32_t P = read_u32_le(&buf[recs[0].last_pgno_off]);
  uint32_t other = read_u32_le(&buf[recs[1].last_pgno_off]);
  if (P == other)
    return -1;
  const size_t victim = (recs[0].count <= recs[1].count) ? 1 : 0;
  const size_t donor = 1 - victim;
  P = read_u32_le(&buf[recs[donor].last_pgno_off]);
  write_u32_le(&buf[recs[victim].last_pgno_off], P);

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return 1;
}

/*---------------------------------------------------------------------------*/

/* Patch one field inside a named-table record (an N_TREE node of the MAIN
 * tree): field_off 8 is tree_t.root, 2 is tree_t.height. Returns true when
 * the record was found and patched. */
bool patch_subtable_field(const std::string &path, uint32_t pagesize, const char *table, size_t field_off,
                          uint32_t value, bool sixteen) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
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

  const size_t klen = std::strlen(table);
  bool patched = false;
  for (size_t off = 3 * pagesize; off + pagesize <= buf.size() && !patched; off += pagesize) {
    if (read_u16_le(&buf[off + 10]) != kPLeaf)
      continue;
    const uint16_t lower = read_u16_le(&buf[off + 12]);
    const size_t nkeys = lower / 2;
    for (size_t i = 0; i < nkeys && !patched; ++i) {
      const uint16_t entry = read_u16_le(&buf[off + kPageHdrSz + 2 * i]);
      uint8_t *node = buf.data() + off + entry + kPageHdrSz;
      const uint16_t nflags = read_u16_le(node + 4);
      const uint16_t ksize = read_u16_le(node + 6);
      if (nflags == kNTree && ksize == klen && std::memcmp(node + 8, table, klen) == 0) {
        uint8_t *field = node + 8 + klen + field_off;
        if (sixteen)
          write_u16_le(field, uint16_t(value));
        else
          write_u32_le(field, value);
        patched = true;
      }
    }
  }
  if (!patched)
    return false;
  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return true;
}

/* A database with a named table holding enough items for a real tree. */
void create_subtable_db(const std::string &path) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path.c_str(), MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "sub", MDBX_CREATE, &dbi));
  for (unsigned i = 0; i < 500; ++i) {
    char key[16], val[64];
    std::snprintf(key, sizeof key, "key-%04u", i);
    std::memset(val, 'a' + (char)(i % 26), sizeof val);
    MDBX_val k = {key, std::strlen(key)}, v = {val, sizeof val};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
}

uint32_t probe_pagesize(const std::string &path) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return 0;
  if (mdbx_env_open(env, path.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return 0;
  }
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  const uint32_t ps = mdbx_env_info_ex(env, nullptr, &info, sizeof info) == MDBX_SUCCESS ? info.mi_dxb_pagesize : 0;
  mdbx_env_close(env);
  return ps;
}

MDBX_env *open_chk_env(const std::string &path) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  if (mdbx_env_set_maxdbs(env, 4) != MDBX_SUCCESS ||
      mdbx_env_open(env, path.c_str(), MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

} // namespace

/*---------------------------------------------------------------------------*/

TEST(chk_verdicts, clean_database_has_no_problems) {
  const env_files_guard guard({"chk-verdicts-clean.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string path = "chk-verdicts-clean.mdbx";
  create_subtable_db(path);

  MDBX_env *env = open_chk_env(path);
  ASSERT_NE(nullptr, env);
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  /* the C API requires a callbacks instance; a zeroed one is silent */
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb);
  const int rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  mdbx_env_close(env);
  ASSERT_EQ(MDBX_SUCCESS, rc);
  EXPECT_EQ(0u, ctx.result.total_problems);
  EXPECT_EQ(0u, ctx.result.problems_meta);
  EXPECT_EQ(0u, ctx.result.problems_gc);
  EXPECT_EQ(0u, ctx.result.problems_kv);
  EXPECT_GE(ctx.result.table_total, 1u) << "the named table must be accounted";
}

TEST(chk_verdicts, page_listed_in_two_gc_records) {
  const env_files_guard guard({"chk-verdicts-gc.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string path = "chk-verdicts-gc.mdbx";
  ASSERT_TRUE(create_gc_db(path));

  const uint32_t pagesize = probe_pagesize(path);
  ASSERT_GE(pagesize, 256u);
  const int crafted = craft_shared_gc_page(path, pagesize);
  if (crafted == 0)
    GTEST_SKIP() << "GC holds fewer than two records; crafted scenario not applicable";
  ASSERT_EQ(1, crafted);

  MDBX_env *env = open_chk_env(path);
  ASSERT_NE(nullptr, env);
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  issue_log log;
  issue_log::s_current = &log;
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb);
  cb.issue = &issue_log::on_issue;
  const int rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  issue_log::s_current = nullptr;
  mdbx_env_close(env);

  ASSERT_EQ(MDBX_PROBLEM, rc) << "a counted problem must surface as MDBX_PROBLEM";
  EXPECT_GE(ctx.result.problems_gc, 1u);
  EXPECT_GE(ctx.result.total_problems, ctx.result.problems_gc);
  bool listed = false;
  for (const std::string &c : log.captions)
    if (c.find("already listed in GC") != std::string::npos)
      listed = true;
  EXPECT_TRUE(listed) << "the duplicate GC page must be reported as already listed in GC";
}

TEST(chk_verdicts, subtable_root_beyond_geometry) {
  const env_files_guard guard({"chk-verdicts-root.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string path = "chk-verdicts-root.mdbx";
  create_subtable_db(path);
  const uint32_t pagesize = probe_pagesize(path);
  ASSERT_GE(pagesize, 256u);
  /* a root far beyond the allocated space, but a legal page number */
  ASSERT_TRUE(patch_subtable_field(path, pagesize, "sub", 8 /* tree_t.root */, 0x30000000u, false));

  MDBX_env *env = open_chk_env(path);
  ASSERT_NE(nullptr, env);
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  issue_log log;
  issue_log::s_current = &log;
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb);
  cb.issue = &issue_log::on_issue;
  const int rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  issue_log::s_current = nullptr;
  mdbx_env_close(env);

  ASSERT_EQ(MDBX_PROBLEM, rc);
  EXPECT_GE(ctx.result.total_problems, 1u);
  bool wrong_pgno = false;
  for (const std::string &c : log.captions)
    if (c.find("page-no") != std::string::npos)
      wrong_pgno = true;
  EXPECT_TRUE(wrong_pgno) << "the out-of-geometry root must be reported as a wrong page-no";
}

TEST(chk_verdicts, subtable_height_mismatch) {
  const env_files_guard guard({"chk-verdicts-height.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string path = "chk-verdicts-height.mdbx";
  create_subtable_db(path);
  const uint32_t pagesize = probe_pagesize(path);
  ASSERT_GE(pagesize, 256u);
  /* two levels above the actual height: the walker descends the real tree
   * and the stored height cannot match */
  ASSERT_TRUE(patch_subtable_field(path, pagesize, "sub", 2 /* tree_t.height */, 9, true));

  MDBX_env *env = open_chk_env(path);
  ASSERT_NE(nullptr, env);
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  issue_log log;
  issue_log::s_current = &log;
  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb);
  cb.issue = &issue_log::on_issue;
  const int rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  issue_log::s_current = nullptr;
  mdbx_env_close(env);

  ASSERT_EQ(MDBX_PROBLEM, rc);
  EXPECT_GE(ctx.result.total_problems, 1u);
  bool height = false;
  for (const std::string &c : log.captions)
    if (c.find("tree height") != std::string::npos)
      height = true;
  EXPECT_TRUE(height) << "the stored/actual height disagreement must be reported";
}
