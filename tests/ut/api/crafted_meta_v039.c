/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression for the defect root-split-cursor-stack-shift-bound of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * page_split()'s root-split adjustment of the other cursors shifted their
 * stacks over levels 0..root_split_height, where root_split_height is the
 * OLD tree height, not the individual cursor's own tracked stack
 * (top + stash). At the maximal height accepted by meta_validate()
 * (height == CURSOR_STACK_SIZE - 1, reachable from a crafted meta) the old
 * loop's last iteration wrote one slot past the cursor's pg[] member - a
 * formally undefined out-of-bounds write, caught by the bounds sanitizer.
 * The fix bounds the loop by m3->top + m3->stash, exactly what the
 * root-collapse shift in tree_rebalance() covers.
 *
 * The crafting (the meta_validate arithmetic accepts height = 31: root is
 * valid, leaf_pages > 0, branch_pages > 0): a real database whose MAIN root
 * is a nearly-full LEAF page (depth == 1), then the meta's height is set to
 * 31 and the branch-page count to 1. The root is a leaf, so the cursor
 * stands on it at top == 0 regardless of the declared height (the descent
 * walks by page type), and the first insert that does not fit splits the
 * root - page_split() takes the root-split branch with
 * root_split_height = 31 and shifts the anchored cursor's stack.
 *
 * Red: on the unfixed code the shift writes m3->pg[CURSOR_STACK_SIZE] -
 * under the bounds sanitizer (ENABLE_UBSAN builds) it traps. Green: with
 * the fix the crafted scenario runs clean. In plain builds both pass
 * (the aliasing write has no lasting effect) - this test doubles as a
 * crash/corruption canary for the crafted open + root-split path.
 * White-box: the crafted meta is built through the real meta_t/meta_sign
 * helpers compiled from the sources. */

#define debug_log debug_log_sub

#include "../../../src/cogs.c"
#include "../../../src/meta.c"
#include "details-anchors.h"

#include <stdarg.h>
static int debug_verbose;
MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  if (!debug_verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "lib[%d] %s:%d: ", level, function ? function : "?", line);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

static const char kDbPath[] = "crafted-v039.mdbx";
static const char kLckPath[] = "crafted-v039.mdbx-lck";

static void cleanup(void) {
  remove(kDbPath);
  remove(kLckPath);
}

/* MDBX_ALIGNAS: the page-cast of a plain uint8_t array relies on the
 * placement the compiler/linker grants, which a strict layout does not
 * (a static uint8_t array may sit at any byte address). */
static MDBX_ALIGNAS(8) uint8_t baseline[MDBX_MAX_PAGESIZE];
static MDBX_ALIGNAS(8) uint8_t work[MDBX_MAX_PAGESIZE];

static const unsigned kMaxHeight = CURSOR_STACK_SIZE - 1; /* 31: the maximal height meta_validate accepts */

static MDBX_env *env_create_open(const char *path, MDBX_env_flags_t flags) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc) {
    printf("root-split-cursor-stack-shift-bound: env_create rc=%d\n", rc);
    return nullptr;
  }
  rc = mdbx_env_open(env, path, flags, 0644);
  if (rc) {
    printf("root-split-cursor-stack-shift-bound: env_open rc=%d (%s)\n", rc, mdbx_strerror(rc));
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

/* builds a real database whose MAIN root is a nearly-full LEAF page
 * (depth == 1): the record count is converged by bisection, since the
 * exact capacity is page-size dependent, then the chosen count is written
 * and committed for real (the crafted meta snapshots THAT database) */
static bool build_database(unsigned count, bool commit, unsigned *depth_out) {
  remove(kDbPath);
  remove(kLckPath);
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env))
    return false;
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644)) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi)) {
    mdbx_env_close(env);
    return false;
  }
  char kbuf[32], vbuf[32];
  for (unsigned i = 0; i < count; ++i) {
    int kn = snprintf(kbuf, sizeof(kbuf), "k%08u", i);
    int vn = snprintf(vbuf, sizeof(vbuf), "v%08u", i);
    MDBX_val key = {kbuf, (size_t)kn};
    MDBX_val value = {vbuf, (size_t)vn};
    if (mdbx_put(txn, dbi, &key, &value, 0)) {
      mdbx_env_close(env);
      return false;
    }
  }
  MDBX_stat stat;
  memset(&stat, 0, sizeof(stat));
  if (mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat))) {
    mdbx_env_close(env);
    return false;
  }
  *depth_out = stat.ms_depth;
  if (commit) {
    if (mdbx_txn_commit(txn)) {
      mdbx_env_close(env);
      return false;
    }
    mdbx_env_close(env);
    return true;
  }
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  return true; /* a probe: the depth is reported via depth_out */
}

static bool build_nearly_full_leaf_root(unsigned *record_count) {
  unsigned low = 8, high = 4096;
  while (low < high) {
    const unsigned try_count = low + (high - low) / 2 + 1;
    unsigned depth = 0;
    if (!build_database(try_count, false, &depth))
      return false;
    if (depth == 1) {
      low = try_count;
      if (high - try_count <= 1)
        break;
    } else {
      high = try_count - 1;
    }
  }
  *record_count = low;
  return low >= 8;
}

static bool write_crafted_meta(unsigned *baseline_pgno) {
  /* snapshot a valid meta page (the corpus pattern) */
  MDBX_env *env = env_create_open(kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY);
  if (!env)
    return false;
  const size_t ps = env->ps;
  bool found = false;
  for (unsigned n = 0; n < NUM_METAS && !found; ++n) {
    const page_t *page = pgno2page(env, (pgno_t)n);
    memcpy(baseline, page, ps);
    if (meta_validate(env, page_meta((page_t *)(void *)baseline), (const page_t *)baseline, n, nullptr) ==
        MDBX_SUCCESS) {
      found = true;
      *baseline_pgno = n;
    }
  }
  /* craft the winner meta slot only: the maximal height + a non-zero
   * branch-page count (the #454 equivalences), the txnid bumped so the
   * crafted slot clearly wins the troika latch, re-signed steady. The
   * in-memory validation runs while the env is still open (meta_validate
   * reads the env's filesize); the crafted page is written to the file
   * after the env is closed. Writing the same meta to all three slots
   * would "clash" the troika. */
  memcpy(work, baseline, ps);
  page_t *page = (page_t *)(void *)work;
  page->pgno = (pgno_t)*baseline_pgno;
  meta_t *meta = page_meta(page);
  meta->trees.main.height = (uint16_t)kMaxHeight;
  meta->trees.main.branch_pages = 1;
  const uint64_t next_txnid = unaligned_peek_u64(4, &meta->txnid_a) + 1;
  unaligned_poke_u64(4, &meta->txnid_a, next_txnid);
  unaligned_poke_u64(4, &meta->txnid_b, next_txnid);
  meta_sign_as_steady(meta);
  {
    debug_verbose = 1;
    const int crafted_rc =
        meta_validate(env, page_meta((page_t *)(void *)work), (const page_t *)work, *baseline_pgno, nullptr);
    debug_verbose = 0;
    if (crafted_rc != MDBX_SUCCESS) {
      printf("root-split-cursor-stack-shift-bound: FAIL - the crafted meta does not validate, rc=%d (see the reject "
             "above)\n",
             crafted_rc);
      mdbx_env_close(env);
      return false;
    }
  }
  mdbx_env_close(env);

  FILE *f = fopen(kDbPath, "r+b");
  if (!f) {
    printf("root-split-cursor-stack-shift-bound: FAIL - could not reopen the file for the crafted meta\n");
    return false;
  }
  fseek(f, (long)*baseline_pgno * (long)ps, SEEK_SET);
  if (fwrite(work, 1, ps, f) != ps) {
    fclose(f);
    printf("root-split-cursor-stack-shift-bound: FAIL - could not write the crafted meta\n");
    return false;
  }
  fclose(f);
  return true;
}

int main(void) {
  unsigned record_count = 0;
  if (!build_nearly_full_leaf_root(&record_count)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - could not build the nearly-full leaf-root database\n");
    {
      cleanup();
      return 1;
    }
  }
  unsigned final_depth = 0;
  if (!build_database(record_count, true, &final_depth)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - could not commit the final database\n");
    {
      cleanup();
      return 1;
    }
  }
  printf(
      "root-split-cursor-stack-shift-bound: the real database: %u records, MAIN root is a nearly-full leaf (depth 1)\n",
      record_count);

  unsigned baseline_pgno = 0;
  if (!write_crafted_meta(&baseline_pgno)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - could not craft the maximal-height meta\n");
    {
      cleanup();
      return 1;
    }
  }

  /* reopen: the crafted meta is accepted, the root leaf is walkable */
  MDBX_env *env = env_create_open(kDbPath, MDBX_NOSUBDIR);
  if (!env) {
    printf("root-split-cursor-stack-shift-bound: FAIL - the crafted database did not reopen\n");
    {
      cleanup();
      return 1;
    }
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - no write transaction on the crafted database\n");
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - the crafted MAIN table did not open\n");
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }

  /* an anchored other cursor: the root-split shift loop walks it */
  MDBX_cursor *anchor = nullptr;
  if (mdbx_cursor_open(txn, dbi, &anchor)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - the anchor cursor did not open\n");
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }
  MDBX_val key = {nullptr, 0}, value = {nullptr, 0};
  if (mdbx_cursor_get(anchor, &key, &value, MDBX_FIRST)) {
    printf("root-split-cursor-stack-shift-bound: FAIL - the anchor cursor did not position\n");
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }

  /* the insert that does not fit the full leaf root: the root split with
   * root_split_height = the crafted maximal height. On the unfixed code the
   * shift loop writes m3->pg[CURSOR_STACK_SIZE] (trapped by the bounds
   * sanitizer); with the fix it is bounded by the cursor's own stack. */
  char big[1024];
  memset(big, 0x7A, sizeof(big)); /* 'z' sorts after every "k%08u" key */
  key.iov_base = (void *)big;
  key.iov_len = 32;
  value.iov_base = (void *)big;
  value.iov_len = sizeof(big);
  const int put_rc = mdbx_put(txn, dbi, &key, &value, 0);
  if (put_rc != MDBX_SUCCESS) {
    printf("root-split-cursor-stack-shift-bound: the crafted insert reported %d (%s)\n", put_rc, mdbx_strerror(put_rc));
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }
  const int commit_rc = mdbx_txn_commit(txn);
  txn = nullptr;
  mdbx_cursor_close(anchor);
  if (commit_rc != MDBX_SUCCESS) {
    printf("root-split-cursor-stack-shift-bound: the crafted-split commit reported %d (%s)\n", commit_rc,
           mdbx_strerror(commit_rc));
    mdbx_env_close(env);
    {
      cleanup();
      return 1;
    }
  }
  mdbx_env_close(env);

  /* both builds commit the split with tree->height == CURSOR_STACK_SIZE,
   * so the crafted database becomes unopenable - that is the campaign's
   * availability-loss note, equal for the unfixed and the fixed code */
  MDBX_env *reopen = env_create_open(kDbPath, MDBX_NOSUBDIR);
  const bool reopen_ok = (reopen != nullptr);
  if (reopen)
    mdbx_env_close(reopen);
  remove(kDbPath);
  remove(kLckPath);

  printf("root-split-cursor-stack-shift-bound: PASS - the crafted maximal-height root split completed "
         "(reopen after the split: %s, the campaign's availability note)\n",
         reopen_ok ? "ok" : "refused as expected");
  return 0;
}
