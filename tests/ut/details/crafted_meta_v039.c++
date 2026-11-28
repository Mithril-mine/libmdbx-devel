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
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression for the defect root-split-cursor-stack-shift-bound of the
 * verification campaign (issue #485): page_split()'s root-split adjustment
 * of the OTHER cursors shifted their stacks over the levels
 * 0..root_split_height, where root_split_height is the OLD tree height. At
 * the maximal height accepted by meta_validate()
 * (height == CURSOR_STACK_SIZE - 1, reachable from a crafted meta) the old
 * loop wrote m3->pg[CURSOR_STACK_SIZE] - under the bounds sanitizer
 * (ENABLE_UBSAN builds) it traps. Green: with the fix the crafted scenario
 * runs clean. In plain builds both pass (the aliasing write has no lasting
 * effect) - this test doubles as a crash/corruption canary for the crafted
 * open + root-split path.
 *
 * The crafting: a real database whose MAIN root is a nearly-full LEAF page
 * (depth 1, found by bisection over the record counts), then the meta's
 * height = the maximal accepted one, branch_pages = 1, the txnid bumped and
 * re-signed steady; a large insert forces the root split.
 *
 * The crafted meta is built through the testing API's layout accessors
 * (tests/AGENTS.md §8, issue #519 stage 5): the field crafting and the
 * on-disk-format knowledge stay inside the testing API, and the crafted
 * meta's verdict is observed through the live mdbx_tst_meta_validate(). */

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*-----------------------------------------------------------------------------*/

namespace {

constexpr const char kDbPath[] = "crafted-v039.mdbx";

/* The union grants the 8-byte alignment (a plain static uint8_t array may
 * sit at any byte address), which the unaligned u64 field access requires. */
union align8_u {
  uint64_t force_align;
  uint8_t bytes[MDBX_MAX_PAGESIZE];
};
align8_u baseline_buf, work_buf;
uint8_t *const baseline = baseline_buf.bytes;
uint8_t *const work = work_buf.bytes;

/* the maximal tree height the engine accepts (the live CURSOR_STACK_SIZE
 * bound, build-dependent - 31 on the 64-bit targets) */
unsigned max_height(void) { return mdbx_tst_cursor_stack_size() - 1; }

MDBX_env *env_create_open(const char *path, MDBX_env_flags_t flags) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc) {
    ADD_FAILURE() << "env_create rc=" << rc;
    return nullptr;
  }
  rc = mdbx_env_open(env, path, flags, 0644);
  if (rc) {
    /* the caller may legitimately expect the refusal (the availability-loss
     * note), so the report is informational, not a failure here */
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
bool build_database(unsigned count, bool commit, unsigned *depth_out) {
  remove(kDbPath);
  MDBX_env *env = nullptr;
  env_handle_guard henv(&env);
  if (mdbx_env_create(&env))
    return false;
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644))
    return false;
  MDBX_txn *txn = nullptr;
  txn_handle_guard htxn(&txn);
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn))
    return false;
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi))
    return false;
  char kbuf[32], vbuf[32];
  for (unsigned i = 0; i < count; ++i) {
    int kn = snprintf(kbuf, sizeof(kbuf), "k%08u", i);
    int vn = snprintf(vbuf, sizeof(vbuf), "v%08u", i);
    MDBX_val key = {kbuf, (size_t)kn};
    MDBX_val value = {vbuf, (size_t)vn};
    if (mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0))
      return false;
  }
  MDBX_stat stat;
  memset(&stat, 0, sizeof(stat));
  if (mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)))
    return false;
  *depth_out = stat.ms_depth;
  if (commit) {
    if (mdbx_txn_commit(txn))
      return false;
    htxn.release();
    return true;
  }
  mdbx_txn_abort(txn);
  htxn.release();
  return true; /* a probe: the depth is reported via depth_out */
}

bool build_nearly_full_leaf_root(unsigned *record_count) {
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

bool write_crafted_meta(unsigned *baseline_pgno) {
  /* snapshot a valid meta page (the corpus pattern) */
  MDBX_env *env = env_create_open(kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY);
  if (!env)
    return false;
  env_handle_guard henv(&env);
  const size_t ps = mdbx_tst_env_pagesize(env);
  bool found = false;
  for (unsigned n = 0; n < MDBX_TST_NUM_METAS && !found; ++n) {
    if (mdbx_tst_env_read_page(env, n, baseline, ps) != MDBX_SUCCESS)
      break;
    if (mdbx_tst_meta_validate(env, baseline, n, nullptr) == MDBX_SUCCESS) {
      found = true;
      *baseline_pgno = n;
    }
  }
  if (!found)
    return false;

  /* craft the winner meta slot only: the maximal height + a non-zero
   * branch-page count (the #454 equivalences), the txnid bumped so the
   * crafted slot clearly wins the troika latch, re-signed steady. The
   * in-memory validation runs while the env is still open (the engine
   * reads the env's filesize); the crafted page is written to the file
   * after the env is closed. Writing the same meta to all three slots
   * would "clash" the troika. */
  memcpy(work, baseline, ps);
  mdbx_tst_page_set_pgno(work, *baseline_pgno);
  mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, max_height());
  mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_BRANCH_PAGES, 1);
  const uint64_t next_txnid = mdbx_tst_meta_field_get(work, MDBX_TST_META_TXNID_A) + 1;
  mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_A, next_txnid);
  mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_B, next_txnid);
  mdbx_tst_meta_sign_as_steady(work);
  /* the verdict from the live mdbx_tst_meta_validate(); the engine's own
   * logging surfaces as is */
  const int crafted_rc = mdbx_tst_meta_validate(env, work, *baseline_pgno, nullptr);
  if (crafted_rc != MDBX_SUCCESS) {
    ADD_FAILURE() << "the crafted meta does not validate, rc=" << crafted_rc << " (see the reject above)";
    return false;
  }
  henv.release();
  mdbx_env_close(env);

  /* the file I/O at the slot's offset: meta slot n lives at n x pagesize */
  FILE *f = fopen(kDbPath, "r+b");
  if (!f) {
    ADD_FAILURE() << "could not reopen the file for the crafted meta";
    return false;
  }
  fseek(f, (long)*baseline_pgno * (long)ps, SEEK_SET);
  const bool written = (fwrite(work, 1, ps, f) == ps);
  fclose(f);
  if (!written) {
    ADD_FAILURE() << "could not write the crafted meta";
    return false;
  }
  return true;
}

} /* namespace */

TEST(crafted_meta_v039, root_split_cursor_stack_shift_bound) {
  env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  unsigned record_count = 0;
  ASSERT_TRUE(build_nearly_full_leaf_root(&record_count)) << "could not build the nearly-full leaf-root database";
  unsigned final_depth = 0;
  ASSERT_TRUE(build_database(record_count, true, &final_depth)) << "could not commit the final database";
  ASSERT_EQ(1u, final_depth) << "the committed MAIN root is not a depth-1 leaf";
  printf(
      "root-split-cursor-stack-shift-bound: the real database: %u records, MAIN root is a nearly-full leaf (depth 1)\n",
      record_count);

  unsigned baseline_pgno = 0;
  ASSERT_TRUE(write_crafted_meta(&baseline_pgno)) << "could not craft the maximal-height meta";

  /* reopen: the crafted meta is accepted, the root leaf is walkable */
  MDBX_env *env = env_create_open(kDbPath, MDBX_NOSUBDIR);
  ASSERT_NE(nullptr, env) << "the crafted database did not reopen";
  env_handle_guard henv(&env);
  MDBX_txn *txn = nullptr;
  txn_handle_guard htxn(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));

  /* an anchored other cursor: the root-split shift loop walks it */
  MDBX_cursor *anchor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &anchor));
  MDBX_val key = {nullptr, 0}, value = {nullptr, 0};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(anchor, &key, &value, MDBX_FIRST));

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
  const int put_rc = mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0);
  ASSERT_EQ(MDBX_SUCCESS, put_rc) << "the crafted insert reported " << put_rc << " (" << mdbx_strerror(put_rc) << ")";
  const int commit_rc = mdbx_txn_commit(txn);
  htxn.release();
  mdbx_cursor_close(anchor);
  ASSERT_EQ(MDBX_SUCCESS, commit_rc) << "the crafted-split commit reported " << commit_rc << " ("
                                     << mdbx_strerror(commit_rc) << ")";
  henv.release();
  mdbx_env_close(env);

  /* both builds commit the split with tree->height == CURSOR_STACK_SIZE,
   * so the crafted database becomes unopenable - that is the campaign's
   * availability-loss note, equal for the unfixed and the fixed code */
  MDBX_env *reopen = env_create_open(kDbPath, MDBX_NOSUBDIR);
  const bool reopen_ok = (reopen != nullptr);
  if (reopen)
    mdbx_env_close(reopen);
  printf("root-split-cursor-stack-shift-bound: PASS - the crafted maximal-height root split completed "
         "(reopen after the split: %s, the campaign's availability note)\n",
         reopen_ok ? "ok" : "refused as expected");
}
