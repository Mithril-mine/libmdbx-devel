#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <thread>

extern "C" int mprobe_ctl(const char *request, char *reply, size_t reply_size);

// Follow-up regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/104
// mdbx_txn_clone() publishes the snapshot in the clone's reader slot while the
// clone is still marked finished. When the source then failed the final check
// (e.g. another thread reset it meanwhile), the bailout reset the clone and
// ro_slot_clean() stopped the process on its ENSURE that a finished
// transaction holds no snapshot; with a parked source the slot also holds the
// parking mark, which must be taken back too. A write transaction passed as
// the clone to reuse was reset as a read-only one. The failing check is injected via the
// probe-bus FAULT site `txn_clone_recheck`, so this requires MDBX_PROBES=ON
// and MDBX_PROBES=1.

TEST(issue_gh0104, clone_bailout_after_publishing_the_snapshot) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue104-clone.mdbx";
  remove(dbname);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
  MDBX_val key = {(void *)"k", 1}, data = {(void *)"v", 1};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, main_dbi, &key, &data, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  MDBX_txn *source = nullptr, *clone = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &source));
  /* Warm-up: probe sites are registered lazily on first fire. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_clone(source, &clone, nullptr));

  char reply[256], cmd[128];
  std::snprintf(cmd, sizeof cmd, "fault txn_clone_recheck %d", (int)MDBX_BAD_TXN);
  ASSERT_EQ(0, mprobe_ctl(cmd, reply, sizeof reply));
  /* Both bailout paths: into the existing clone, and into a new one. */
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_clone(source, &clone, nullptr));
  ASSERT_EQ(0, mprobe_ctl(cmd, reply, sizeof reply));
  MDBX_txn *fresh = nullptr;
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_clone(source, &fresh, nullptr));
  EXPECT_EQ(nullptr, fresh);
  ASSERT_EQ(0, mprobe_ctl("fault txn_clone_recheck none", reply, sizeof reply));

  /* The clone handle is reusable afterwards. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_clone(source, &clone, nullptr));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(clone, main_dbi, &key, &data));

  /* A parked source: the clone's slot also gets the parking mark, which the
   * bailout must take back together with the snapshot. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(source, false));
  ASSERT_EQ(0, mprobe_ctl(cmd, reply, sizeof reply));
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_clone(source, &clone, nullptr));
  ASSERT_EQ(0, mprobe_ctl("fault txn_clone_recheck none", reply, sizeof reply));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_renew(clone)) << "the clone's slot must be left clean";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(clone, main_dbi, &key, &data));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(clone));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(source, false));

  /* A write transaction as the clone to reuse is refused and left intact. */
  MDBX_txn *writer = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &writer));
  EXPECT_EQ(MDBX_EINVAL, mdbx_txn_clone(source, &writer, nullptr));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_put(writer, main_dbi, &key, &data, MDBX_UPSERT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(writer));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(source));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  remove(dbname);
}

static int count_parked_idle_slot(void *ctx, int, int, mdbx_pid_t, mdbx_tid_t thread, uint64_t txnid, uint64_t, size_t,
                                  size_t) noexcept {
  /* a slot without a snapshot whose thread id is still the parking mark (all ones) */
  const unsigned char *const tid = reinterpret_cast<const unsigned char *>(&thread);
  bool all_ones = true;
  for (size_t i = 0; i < sizeof(thread); ++i)
    all_ones &= tid[i] == 0xFF;
  if (txnid == 0 && all_ones)
    ++*static_cast<int *>(ctx);
  return 0;
}

static int count_parked_idle_slots(MDBX_env *env) {
  int count = 0;
  mdbx_reader_list(env, count_parked_idle_slot, &count);
  return count;
}

TEST(issue_gh0104, clone_bailout_of_parked_source_on_another_thread) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* Default (sticky) threads: the clone uses the worker thread's own slot,
   * which must get back its thread id after the bailout. */
  const char *const dbname = "issue104-clone-sticky.mdbx";
  remove(dbname);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR, 0644));
  MDBX_txn *source = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &source));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(source, false));

  int warm = -1, faulted = -1, prepared = -1, begun = -1, parked_idle = -1;
  std::thread worker([&] {
    MDBX_txn *clone = nullptr;
    warm = mdbx_txn_clone(source, &clone, nullptr);
    if (warm == MDBX_SUCCESS)
      mdbx_txn_abort(clone);
    char reply[256], cmd[128];
    std::snprintf(cmd, sizeof cmd, "fault txn_clone_recheck %d", (int)MDBX_BAD_TXN);
    clone = nullptr;
    if (mprobe_ctl(cmd, reply, sizeof reply) == 0)
      faulted = mdbx_txn_clone(source, &clone, nullptr);
    mprobe_ctl("fault txn_clone_recheck none", reply, sizeof reply);
    parked_idle = count_parked_idle_slots(env);
    /* prepare-only reuse: the slot must be clean enough for it (the bailout
     * has to clear snapshot_pages_used, not only the snapshot id). */
    MDBX_txn *prepare = nullptr;
    prepared = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY | MDBX_TXN_RDONLY_PREPARE, &prepare);
    if (prepared == MDBX_SUCCESS)
      mdbx_txn_abort(prepare);
    MDBX_txn *reader = nullptr;
    begun = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader);
    if (begun == MDBX_SUCCESS)
      mdbx_txn_abort(reader);
  });
  worker.join();
  EXPECT_EQ(MDBX_SUCCESS, warm);
  EXPECT_EQ(MDBX_BAD_TXN, faulted);
  EXPECT_EQ(0, parked_idle) << "the worker's slot must not keep the parking mark";
  EXPECT_EQ(MDBX_SUCCESS, prepared) << "prepare-only reuse of the slot must work";
  EXPECT_EQ(MDBX_SUCCESS, begun) << "the worker's slot must be usable again";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(source, false));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(source));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  remove(dbname);
}
