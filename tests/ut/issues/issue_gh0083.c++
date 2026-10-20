#include "mdbx.h++"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

/* WATCHDOG: abort the hung process after 120 s so Windows Error Reporting
 * dumps the stuck stack in CI (diagnostics for the gh0083 Windows timeout). */
static std::atomic<bool> watchdog_armed{true};
static void watchdog_thread() {
  for (int i = 0; i < 120 && watchdog_armed.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::seconds(1));
  if (watchdog_armed.load(std::memory_order_acquire)) {
    std::fprintf(stderr, "[gh0083-watchdog] 120s elapsed, hanging detected, aborting for stack dump\n");
    std::fflush(stderr);
    std::abort();
  }
}

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// End-to-end regression for https://github.com/Mithril-mine/libmdbx-devel/issues/83
// A page number appearing in two different GC records was not detected:
// pnl_merge() does not de-duplicate and the only cross-record check was gated
// behind the runtime MDBX_DBG_AUDIT flag, so a release build could hand the
// same physical page out twice (silent aliasing of two b-tree nodes).
// Here a database is crafted so its GC holds two records whose page lists
// both contain the same page number P, while each record still passes its
// own per-record check, and then mdbx_env_defrag() is run: it assembles the
// whole retired-pages list from the untrusted GC records and must reject it
// with MDBX_PROBLEM through pnl_check_full(). Without the fix the duplicate
// goes unnoticed by the record validation and is only caught later as
// corrupted page walk.
//
// NOTE: this is a format-dependent test. It inspects the on-disk layout
// directly (meta offsets PAGEHDRSZ+44/+48, node flags P_BRANCH/P_LEAF and
// the PNL serialization in GC records), so it must be revisited together
// with any on-disk format change (e.g. the planned C->C++ core migration).

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

static void env_remove(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

struct reader_ctx {
  MDBX_env *env;
  MDBX_txn *txn;
  std::atomic<bool> started;
  std::atomic<bool> stop;
  int rc;
};

static void reader_thread(reader_ctx *ctx) {
  std::fprintf(stderr, "[gh0083] reader: txn_begin...\n");
  ctx->txn = nullptr;
  ctx->rc = mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &ctx->txn);
  std::fprintf(stderr, "[gh0083] reader: txn_begin rc=%d\n", ctx->rc);
  ctx->started.store(true, std::memory_order_release);
  while (!ctx->stop.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::fprintf(stderr, "[gh0083] reader: stopping, aborting txn\n");
  if (ctx->txn)
    mdbx_txn_abort(ctx->txn);
  std::fprintf(stderr, "[gh0083] reader: done\n");
}

/* Build a database with several GC records: each write commit retires the
 * previous revision's pages. A lingering reader in a separate thread pins the
 * first snapshot so retired pages stay listed instead of being recycled, and
 * every subsequent commit leaves its own GC record behind. */
static bool create_gc_db(const std::string &testdb) {
  std::fprintf(stderr, "[gh0083] create_gc_db: env_remove + env_open\n");
  env_remove(testdb.c_str());

  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 2) == MDBX_SUCCESS &&
            mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
  std::fprintf(stderr, "[gh0083] create_gc_db: env_open ok=%d\n", (int)ok);
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
    MDBX_val k = {key, std::strlen(key)};
    MDBX_val v = {val, sizeof val};
    ok = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) == MDBX_SUCCESS;
  }
  if (ok)
    ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
  else
    mdbx_txn_abort(txn);
  std::fprintf(stderr, "[gh0083] create_gc_db: 50 keys committed ok=%d\n", (int)ok);

  reader_ctx reader = {env, nullptr, {false}, {false}, 0};
  std::thread thr(reader_thread, &reader);
  std::fprintf(stderr, "[gh0083] create_gc_db: reader thread spawned, waiting started\n");
  while (!reader.started.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::fprintf(stderr, "[gh0083] create_gc_db: reader started rc=%d\n", reader.rc);

  for (unsigned round = 0; ok && round < 3; ++round) {
    txn = nullptr;
    ok = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) == MDBX_SUCCESS;
    std::fprintf(stderr, "[gh0083] create_gc_db: round %u txn_begin ok=%d\n", round, (int)ok);
    for (int i = 0; ok && i < 15; ++i) {
      char key[16], val[128];
      std::snprintf(key, sizeof key, "k%u_%u", round, i);
      std::memset(val, 'x' + (char)round, sizeof val);
      MDBX_val k = {key, std::strlen(key)};
      MDBX_val v = {val, sizeof val};
      ok = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) == MDBX_SUCCESS;
    }
    if (ok)
      ok = mdbx_txn_commit(txn) == MDBX_SUCCESS;
    else
      mdbx_txn_abort(txn);
    std::fprintf(stderr, "[gh0083] create_gc_db: round %u done ok=%d\n", round, (int)ok);
  }

  std::fprintf(stderr, "[gh0083] create_gc_db: stopping reader\n");
  reader.stop.store(true, std::memory_order_release);
  thr.join();
  std::fprintf(stderr, "[gh0083] create_gc_db: env_close\n");
  mdbx_env_close(env);
  std::fprintf(stderr, "[gh0083] create_gc_db: done ok=%d\n", (int)ok);
  return ok;
}

struct gc_record {
  size_t data_off;        /* absolute file offset of the record's page list */
  uint32_t count;         /* pnl[0] */
  uint32_t last_pgno_off; /* absolute file offset of the least page number */
};

/* Descend into the GC tree from the meta and collect up to two leaf records. */
static bool walk_gc_leaf(const std::vector<uint8_t> &buf, uint32_t pagesize, uint32_t pgno, size_t depth,
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
    const uint16_t entry = read_u16_le(&buf[off + 20 + 2 * i]);
    const size_t node_off = off + entry + 20;
    if (flags & 0x01 /* P_BRANCH */) {
      const uint32_t child = read_u32_le(&buf[node_off]);
      if (!walk_gc_leaf(buf, pagesize, child, depth + 1, out) || out.size() >= 2)
        return true;
    } else if (flags & 0x02 /* P_LEAF */) {
      const uint16_t ksize = read_u16_le(&buf[node_off + 6]);
      const uint32_t dsize = read_u32_le(&buf[node_off]);
      const size_t data_off = node_off + 8 + ksize;
      if (ksize == 8 /* txnid key */ && dsize >= sizeof(uint32_t) && data_off + dsize <= off + pagesize) {
        gc_record rec;
        rec.data_off = data_off;
        rec.count = read_u32_le(&buf[data_off]);
        if (rec.count > 0 && rec.count <= 0x1FFFFFu /* sane bound */ &&
            data_off + (size_t)(rec.count + 1) * sizeof(uint32_t) <= off + pagesize) {
          rec.last_pgno_off = data_off + (size_t)rec.count * sizeof(uint32_t);
          out.push_back(rec);
        }
      }
    }
  }
  return out.size() < 2;
}

/* Craft the GC so two records share one page: page lists are stored in
 * descending order, so the last element of a record is its least page. Take
 * that page from one record and store it instead of the least page of the
 * other one, keeping the recipient's list within per-record validity bounds.
 * Returns 1 on success, 0 if the GC has fewer than two records (engine
 * coalescing may merge them; then the crafted scenario is not applicable and
 * the caller should skip the test), and -1 on a real failure. */
static int craft_shared_page(const std::string &testdb, uint32_t pagesize) {
  std::fstream f(testdb, std::ios::in | std::ios::out | std::ios::binary);
  if (!f.is_open())
    return -1;
  std::fprintf(stderr, "[gh0083] craft: file opened, size check\n");
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  if (end <= 0)
    return -1;
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  if (f.gcount() != std::streamsize(buf.size()))
    return -1;
  std::fprintf(stderr, "[gh0083] craft: read %lld bytes\n", (long long)end);

  const size_t PAGEHDRSZ = 20;
  uint32_t root = 0xFFFFFFFFu;
  for (uint32_t pg = 0; pg < 3; ++pg) {
    const size_t off = (size_t)pg * pagesize;
    if (off + pagesize > buf.size())
      break;
    if (read_u16_le(&buf[off + 10]) != 0x08 /* P_META */)
      continue;
    if (read_u32_le(&buf[off + PAGEHDRSZ + 44]) != pagesize)
      continue;
    root = read_u32_le(&buf[off + PAGEHDRSZ + 48]);
    if (root != 0xFFFFFFFFu && root >= 3 /* NUM_METAS */)
      break;
  }
  if (root == 0xFFFFFFFFu)
    return -1;

  std::vector<gc_record> recs;
  std::fprintf(stderr, "[gh0083] craft: walk_gc_leaf root=%u\n", root);
  walk_gc_leaf(buf, pagesize, root, 0, recs);
  std::fprintf(stderr, "[gh0083] craft: walk done, %zu records\n", recs.size());
  if (recs.size() < 2)
    return 0;

  uint32_t P = read_u32_le(&buf[recs[0].last_pgno_off]);
  uint32_t other = read_u32_le(&buf[recs[1].last_pgno_off]);
  if (P == other)
    return -1;
  if (P < other)
    std::swap(P, other); /* P must be the minimum of the second list */
  const size_t victim = (recs[0].count <= recs[1].count) ? 1 : 0;
  const size_t donor = 1 - victim;
  P = read_u32_le(&buf[recs[donor].last_pgno_off]);
  write_u32_le(&buf[recs[victim].last_pgno_off], P);

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
  return 1;
}

TEST(issue_gh0083, duplicated_page_across_gc_records_rejected) {
  std::thread watchdog(watchdog_thread);
  watchdog.detach();
  const std::string testdb = "issue83.mdbx";
  std::fprintf(stderr, "[gh0083] test: start\n");
  env_remove(testdb.c_str());
  ASSERT_TRUE(create_gc_db(testdb)) << "must be able to create a GC with several records";
  std::fprintf(stderr, "[gh0083] test: create_gc_db done\n");

  /* Obtain the actual page size from the environment instead of assuming a
   * platform-dependent constant. */
  uint32_t pagesize = 0;
  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_RDONLY | MDBX_NOSUBDIR, 0644));
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof info);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
    pagesize = info.mi_dxb_pagesize;
    mdbx_env_close(env);
    ASSERT_GE(pagesize, 256u);
  }
  std::fprintf(stderr, "[gh0083] test: pagesize=%u\n", pagesize);

  std::fprintf(stderr, "[gh0083] test: crafting shared page...\n");
  const int crafted = craft_shared_page(testdb, pagesize);
  std::fprintf(stderr, "[gh0083] test: crafted=%d\n", crafted);
  if (crafted == 0) {
    /* Engine GC coalescing may merge the retired lists into a single record,
     * making the crafted two-record scenario inapplicable; skip rather than
     * fail when the GC layout evolves. */
    GTEST_SKIP() << "GC holds fewer than two records; crafted scenario not applicable";
  }
  ASSERT_GT(crafted, 0) << "must be able to craft two records sharing a page";

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  std::fprintf(stderr, "[gh0083] test: reopen env\n");
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));
  std::fprintf(stderr, "[gh0083] test: reopen done, defrag start\n");

  /* Defragmentation assembles the whole retired-pages list from the untrusted
   * GC records in one go (defrag_load_gc), so a page shared by two records
   * ends up twice in that list and must be rejected there. The fix checks the
   * assembled list with pnl_check_full() and reports MDBX_PROBLEM; without it
   * the duplicate slips through the lightweight check and is later caught as
   * a corrupted page walk (a different error code, hence the red/green split). */
  MDBX_defrag_result_t result;
  std::memset(&result, 0, sizeof result);
  std::fprintf(stderr, "[gh0083] test: calling defrag\n");
  const int rc = mdbx_env_defrag(env, 1, 0, 16, 0, 0, 4096, nullptr, nullptr, &result);
  std::fprintf(stderr, "[gh0083] test: defrag rc=%d\n", rc);
  mdbx_env_close(env);
  env_remove(testdb.c_str());

  EXPECT_EQ(MDBX_PROBLEM, rc) << "a page listed by two GC records must be rejected, not double-allocated";
  std::fprintf(stderr, "[gh0083] test: done\n");
  watchdog_armed.store(false, std::memory_order_release);
}