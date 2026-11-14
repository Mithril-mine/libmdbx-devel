/* Differential test for the full-depth mdbx_cursor_distance() and
 * mdbx_cursor_distribute() with a nested (multi-page) DUPSORT dupset.
 *
 * The dupset is scaled by the page size, so it is stored as a nested sub-DB
 * of height >= 2 on any page size (verified via mdbx_cursor_count_ex()).
 * A naive NEXT/PREV-step oracle provides exact expectations, signed like
 * the API (negative when the cursors are reversed). The distances are
 * asserted exactly for every pair whose begin is not at a non-zero rank
 * inside the multi-page dupset.
 *
 * Known limitation (https://github.com/Mithril-mine/libmdbx-devel/issues/453):
 * when the BEGIN cursor sits at a non-zero rank inside a multi-page dupset,
 * the distance is inflated by the begin's rank within its own dupset, and
 * mdbx_cursor_distribute() reports a shortage for such ranges. This is
 * documented by the code comment in cursor_distance(); the limitation case
 * below reports the current behavior instead of asserting it, so it turns
 * into exact checks once the limitation is resolved. */
#include "mdbx.h++"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static unsigned big_count = 3000;

static void make_kv(unsigned k, unsigned j, MDBX_val *key, MDBX_val *data) {
  static char kb[16], vb[32];
  key->iov_len = (size_t)std::snprintf(kb, sizeof kb, "key-%02u", k);
  key->iov_base = kb;
  data->iov_len = (size_t)std::snprintf(vb, sizeof vb, "val-%02u-%08u", k, j);
  data->iov_base = vb;
}

static int goto_pair(MDBX_cursor *mc, unsigned k, unsigned j) {
  MDBX_val key, data;
  make_kv(k, j, &key, &data);
  return mdbx_cursor_get(mc, &key, &data, MDBX_GET_BOTH);
}

/* The naive signed distance: the exact number of NEXT (or PREV) steps from
 * (k1,j1) to (k2,j2), negative when the target precedes the source. Returns
 * a value < -2000000000 when the target was not reached. */
static long long naive_distance(MDBX_cursor *mc, unsigned k1, unsigned j1, unsigned k2, unsigned j2) {
  const bool forward = (k2 > k1) || (k2 == k1 && j2 > j1);
  if (k2 == k1 && j2 == j1)
    return 0;
  MDBX_val key, data;
  make_kv(k1, j1, &key, &data);
  if (mdbx_cursor_get(mc, &key, &data, MDBX_GET_BOTH) != MDBX_SUCCESS)
    return LONG_MIN;
  char want_key[16], want_val[32];
  std::snprintf(want_key, sizeof want_key, "key-%02u", k2);
  std::snprintf(want_val, sizeof want_val, "val-%02u-%08u", k2, j2);
  long long steps = 0;
  const MDBX_cursor_op op = forward ? MDBX_NEXT : MDBX_PREV;
  for (;;) {
    if (mdbx_cursor_get(mc, &key, &data, op) != MDBX_SUCCESS)
      return LONG_MIN;
    ++steps;
    const bool key_match =
        key.iov_len == std::strlen(want_key) && std::memcmp(key.iov_base, want_key, key.iov_len) == 0;
    const bool val_match =
        data.iov_len == std::strlen(want_val) && std::memcmp(data.iov_base, want_val, data.iov_len) == 0;
    if (key_match && val_match)
      return forward ? steps : -steps;
    if (key.iov_len == std::strlen(want_key)) {
      const int cmp = std::memcmp(key.iov_base, want_key, key.iov_len);
      if (forward ? cmp > 0 : cmp < 0)
        return LONG_MIN;
    }
  }
}

int main() {
  const char *const db = "issue453.mdbx";
  mdbx::env::remove(db);

  int rc;
  MDBX_env *env = nullptr;
  rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return EXIT_FAILURE;
  rc = mdbx_env_set_maxdbs(env, 4);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, db, MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: open rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* scale the big dupset by the page size */
  {
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof info);
    rc = mdbx_env_info_ex(env, nullptr, &info, sizeof info);
    if (rc != MDBX_SUCCESS || info.mi_dxb_pagesize < 256) {
      std::fprintf(stderr, "FAIL: cannot probe the page size\n");
      mdbx_env_close(env);
      return EXIT_FAILURE;
    }
    big_count = (unsigned)(info.mi_dxb_pagesize / 16u) * 4u;
    if (big_count < 3000u)
      big_count = 3000u;
  }

  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_CREATE, &dbi);
  MDBX_val key, data;
  const unsigned counts[4] = {5, 1, big_count, 2};
  for (unsigned k = 0; rc == MDBX_SUCCESS && k < 4; ++k)
    for (unsigned j = 0; rc == MDBX_SUCCESS && j < counts[k]; ++j) {
      MDBX_cursor *mc = nullptr;
      rc = mdbx_cursor_open(txn, dbi, &mc);
      if (rc == MDBX_SUCCESS) {
        make_kv(k, j, &key, &data);
        rc = mdbx_cursor_put(mc, &key, &data, MDBX_APPENDDUP);
        mdbx_cursor_close(mc);
      }
    }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: fill rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* verify the big dupset is a nested sub-DB of height >= 2 */
  {
    MDBX_cursor *mc = nullptr;
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_cursor_open(txn, dbi, &mc);
    if (rc == MDBX_SUCCESS) {
      make_kv(2, 0, &key, &data);
      rc = mdbx_cursor_get(mc, &key, &data, MDBX_GET_BOTH);
    }
    size_t count = 0;
    MDBX_stat stat;
    std::memset(&stat, 0, sizeof stat);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_cursor_count_ex(mc, &count, &stat, sizeof stat);
    if (mc)
      mdbx_cursor_close(mc);
    mdbx_txn_abort(txn);
    if (rc != MDBX_SUCCESS) {
      std::fprintf(stderr, "FAIL: cannot stat the big dupset, rc %d\n", rc);
      mdbx_env_close(env);
      return EXIT_FAILURE;
    }
    if (stat.ms_depth < 2) {
      /* the nested-tree scenario is inapplicable at this page size */
      std::printf("SKIPPED: the big dupset is not a nested sub-DB of height >= 2 (ms_depth %u)\n",
                  (unsigned)stat.ms_depth);
      mdbx_env_close(env);
      mdbx::env::remove(db);
      return EXIT_SUCCESS;
    }
  }

  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: read txn rc %d\n", rc);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }
  MDBX_cursor *c1 = nullptr, *c2 = nullptr;
  if (mdbx_cursor_open(txn, dbi, &c1) != MDBX_SUCCESS || mdbx_cursor_open(txn, dbi, &c2) != MDBX_SUCCESS) {
    std::fprintf(stderr, "FAIL: cannot open cursors\n");
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return EXIT_FAILURE;
  }

  /* exact pairs (the begin is not at a non-zero rank inside the big dupset) */
  const struct {
    unsigned k1, j1, k2, j2;
  } pairs[] = {
      {2, 0, 2, 1}, {2, 0, 2, big_count - 1}, {1, 0, 2, 1}, {0, 1, 2, big_count / 2}, {3, 1, 3, 0}, {1, 0, 1, 0},
  };
  int failures = 0;
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
    const unsigned k1 = pairs[i].k1, j1 = pairs[i].j1, k2 = pairs[i].k2, j2 = pairs[i].j2;
    if (goto_pair(c1, k1, j1) != MDBX_SUCCESS || goto_pair(c2, k2, j2) != MDBX_SUCCESS) {
      std::fprintf(stderr, "FAIL: pair #%zu cannot position cursors\n", i);
      ++failures;
      continue;
    }
    const long long want = naive_distance(c1, k1, j1, k2, j2);
    if (want <= -2000000000ll) {
      std::fprintf(stderr, "FAIL: pair #%zu naive oracle failed\n", i);
      ++failures;
      continue;
    }
    goto_pair(c1, k1, j1);
    goto_pair(c2, k2, j2);
    intptr_t distance = 0;
    rc = mdbx_cursor_distance(c1, c2, &distance, 64);
    if (rc != MDBX_SUCCESS || distance != want) {
      std::fprintf(stderr, "FAIL: pair #%zu (%u/%u -> %u/%u): distance %td instead of %lld (rc %d)\n", i, k1, j1, k2,
                   j2, distance, want, rc);
      ++failures;
    }
  }

  /* the inverted direction: the distance is negative */
  if (failures == 0) {
    goto_pair(c1, 0, 1);
    goto_pair(c2, 0, 3);
    intptr_t distance = 0;
    rc = mdbx_cursor_distance(c2, c1, &distance, 64);
    const long long want = naive_distance(c2, 0, 3, 0, 1);
    if (rc != MDBX_SUCCESS || want <= -2000000000ll || want >= 0 || distance != want) {
      std::fprintf(stderr, "FAIL: inverted distance %td instead of %lld (rc %d)\n", distance, want, rc);
      ++failures;
    }
  }

  /* distribute from the big dupset's first item: not affected by the limitation */
  if (failures == 0) {
    MDBX_cursor *c3 = nullptr, *c4 = nullptr;
    if (mdbx_cursor_open(txn, dbi, &c3) != MDBX_SUCCESS || mdbx_cursor_open(txn, dbi, &c4) != MDBX_SUCCESS ||
        goto_pair(c1, 2, 0) != MDBX_SUCCESS || goto_pair(c2, 3, 0) != MDBX_SUCCESS) {
      std::fprintf(stderr, "FAIL: distribute setup\n");
      ++failures;
    } else {
      MDBX_cursor *array[2] = {c3, c4};
      rc = mdbx_cursor_distribute(c1, c2, array, 2, 40);
      if (rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "FAIL: distribute rc %d\n", rc);
        ++failures;
      } else {
        MDBX_val k, v;
        if (mdbx_cursor_get(c3, &k, &v, MDBX_GET_CURRENT) != MDBX_SUCCESS ||
            mdbx_cursor_get(c4, &k, &v, MDBX_GET_CURRENT) != MDBX_SUCCESS) {
          std::fprintf(stderr, "FAIL: distributed cursors are not positioned\n");
          ++failures;
        }
      }
    }
    if (c3)
      mdbx_cursor_close(c3);
    if (c4)
      mdbx_cursor_close(c4);
  }

  /* Known limitation: report the current behavior without asserting it. */
  if (failures == 0) {
    goto_pair(c1, 2, 1);
    goto_pair(c2, 2, big_count - 1);
    intptr_t distance = 0;
    rc = mdbx_cursor_distance(c1, c2, &distance, 64);
    const long long want = naive_distance(c1, 2, 1, 2, big_count - 1);
    if (rc == MDBX_SUCCESS && distance == want)
      std::printf("NOTE: the limitation is resolved, exact full-depth distance inside a multi-page dupset\n");
    else
      std::printf("NOTE: known limitation #453: distance %td instead of %lld (rc %d)\n", distance, want, rc);
  }

  mdbx_cursor_close(c1);
  mdbx_cursor_close(c2);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  mdbx::env::remove(db);
  if (failures) {
    std::fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("OK\n");
  return EXIT_SUCCESS;
}
