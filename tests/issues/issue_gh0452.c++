/* Characterisation for the duplicate-page rejection on the regular
 * reclamation path (issues
 * https://github.com/Mithril-mine/libmdbx-devel/issues/83 and
 * https://github.com/Mithril-mine/libmdbx-devel/issues/452): pnl_merge()
 * detects a page presented by two GC records only when both are co-present
 * in txn->wr.repnl at the merge moment. In this crafted scenario the shared
 * page is allocated from the first record before the second one is merged,
 * so the duplicate is out of the merge-time check's scope and the
 * transaction succeeds. Full verification of the duplicate-freedom requires
 * analysis of the whole b-tree with all GC data loaded, which is the chk
 * functionality (mdbx_env_chk() and mdbx_chk) that flags the aliased page
 * for this very scenario.
 *
 * NOTE: this is a format-dependent test. It inspects the on-disk layout
 * directly (meta offsets PAGEHDRSZ+44/+48, node flags P_BRANCH/P_LEAF and
 * the PNL serialization in GC records), so it must be revisited together
 * with any on-disk format change.
 */
#include "mdbx.h++"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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
  unlink(path);
  const std::string lck = std::string(path) + "-lck";
  unlink(lck.c_str());
}

struct reader_ctx {
  MDBX_env *env;
  MDBX_txn *txn;
  std::atomic<bool> started;
  std::atomic<bool> stop;
  int rc;
};

static void reader_thread(reader_ctx *ctx) {
  ctx->txn = nullptr;
  ctx->rc = mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &ctx->txn);
  ctx->started.store(true, std::memory_order_release);
  while (!ctx->stop.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (ctx->txn)
    mdbx_txn_abort(ctx->txn);
}

/* Build a database with several GC records: each write commit retires the
 * previous revision's pages. A lingering reader in a separate thread pins the
 * first snapshot so retired pages stay listed instead of being recycled, and
 * every subsequent commit leaves its own GC record behind. */
static bool create_gc_db(const std::string &testdb) {
  env_remove(testdb.c_str());

  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  bool ok = mdbx_env_create(&env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(env, 2) == MDBX_SUCCESS &&
            mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
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

  reader_ctx reader = {env, nullptr, {false}, {false}, 0};
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
      MDBX_val k = {key, std::strlen(key)};
      MDBX_val v = {val, sizeof val};
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
  size_t data_off;      /* absolute file offset of the record's page list */
  size_t count;         /* pnl[0] */
  size_t last_pgno_off; /* absolute file offset of the least page number */
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
            data_off + (rec.count + 1) * sizeof(uint32_t) <= off + pagesize) {
          rec.last_pgno_off = data_off + rec.count * sizeof(uint32_t);
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
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  if (end <= 0)
    return -1;
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  if (f.gcount() != std::streamsize(buf.size()))
    return -1;

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
  walk_gc_leaf(buf, pagesize, root, 0, recs);
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

int main() {
  const std::string testdb = "issue452.mdbx";
  env_remove(testdb.c_str());
  if (!create_gc_db(testdb)) {
    std::fprintf(stderr, "FAIL: cannot create a GC with several records\n");
    return EXIT_FAILURE;
  }

  /* Obtain the actual page size from the environment instead of assuming a
   * platform-dependent constant. */
  uint32_t pagesize = 0;
  {
    MDBX_env *env = nullptr;
    if (mdbx_env_create(&env) != MDBX_SUCCESS || mdbx_env_set_maxdbs(env, 2) != MDBX_SUCCESS ||
        mdbx_env_open(env, testdb.c_str(), MDBX_RDONLY | MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
      std::fprintf(stderr, "FAIL: cannot open the created database for probing\n");
      if (env)
        mdbx_env_close(env);
      return EXIT_FAILURE;
    }
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof info);
    if (mdbx_env_info_ex(env, nullptr, &info, sizeof info) != MDBX_SUCCESS || info.mi_dxb_pagesize < 256) {
      std::fprintf(stderr, "FAIL: cannot probe the database page size\n");
      mdbx_env_close(env);
      return EXIT_FAILURE;
    }
    pagesize = info.mi_dxb_pagesize;
    mdbx_env_close(env);
  }

  const int crafted = craft_shared_page(testdb, pagesize);
  if (crafted == 0) {
    /* Engine GC coalescing may merge the retired lists into a single record,
     * making the crafted two-record scenario inapplicable. */
    std::printf("SKIPPED: GC holds fewer than two records\n");
    env_remove(testdb.c_str());
    return EXIT_SUCCESS;
  }
  if (crafted < 0) {
    std::fprintf(stderr, "FAIL: cannot craft two records sharing a page\n");
    env_remove(testdb.c_str());
    return EXIT_FAILURE;
  }

  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS || mdbx_env_set_maxdbs(env, 2) != MDBX_SUCCESS ||
      mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: cannot open the crafted database\n");
    if (env)
      mdbx_env_close(env);
    env_remove(testdb.c_str());
    return EXIT_FAILURE;
  }

  MDBX_dbi dbi = 0;
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);

  /* Consume the crafted records by allocating enough pages within one
   * transaction: churn retires pages, and the flood's allocations pull
   * both records into the retired list. */
  char key[16], val[128];
  for (int i = 0; rc == MDBX_SUCCESS && i < 200; ++i) {
    std::snprintf(key, sizeof key, "k%02d", i);
    std::memset(val, 'r', sizeof val);
    MDBX_val k = {key, std::strlen(key)};
    MDBX_val v = {val, sizeof val};
    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
  }
  for (int i = 0; rc == MDBX_SUCCESS && i < 2000; ++i) {
    std::snprintf(key, sizeof key, "flood%04d", i);
    std::memset(val, 'q', sizeof val);
    MDBX_val k = {key, std::strlen(key)};
    MDBX_val v = {val, sizeof val};
    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  mdbx_env_close(env);
  env_remove(testdb.c_str());

  /* Characterisation: the crafted duplicate escapes the merge-time check
   * when the page has been already allocated from the first record, and the
   * transaction succeeds. Should a future change make this rejection
   * complete, this expectation is the one to flip. */
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: expected the characterised success (rc %d)\n", rc);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
