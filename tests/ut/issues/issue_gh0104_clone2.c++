#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstring>
#include <string>

// Follow-up regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/104
// A parked read transaction may be ousted by a writer, which then recycles the
// pages of its snapshot. mdbx_txn_clone() skipped its checks for a parked
// origin, so the clone of an ousted origin succeeded, its unpark succeeded too,
// and it read recycled pages (MDBX_CORRUPTED, then an ENSURE on abort). Also a
// clone handle of another environment, or the origin itself, was accepted.

namespace {

constexpr int nkeys = 64;
constexpr size_t vlen = 2000;

int put_round(MDBX_env *env, MDBX_dbi dbi, char fill) {
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    return rc;
  char vbuf[vlen], kbuf[16];
  std::memset(vbuf, fill, vlen);
  for (int i = 0; i < nkeys && rc == MDBX_SUCCESS; ++i) {
    MDBX_val key = {kbuf, size_t(std::snprintf(kbuf, sizeof kbuf, "k%04d", i))}, data = {vbuf, vlen};
    rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
  }
  if (rc != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return rc;
  }
  return mdbx_txn_commit(txn);
}

void remove_db(const char *dbname) {
  std::string lck(dbname);
  lck += "-lck";
  remove(dbname);
  remove(lck.c_str());
}

MDBX_env *open_env(const char *dbname, MDBX_dbi *dbi) {
  remove_db(dbname);
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  /* a small upper bound, so the writer has to recycle the pages of old snapshots */
  MDBX_txn *txn = nullptr;
  if (mdbx_env_set_geometry(env, -1, 64 << 10, 2 << 20, 64 << 10, -1, 4096) != MDBX_SUCCESS ||
      mdbx_env_open(env, dbname, MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS, 0644) != MDBX_SUCCESS ||
      mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  const int rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, dbi);
  if (rc != MDBX_SUCCESS)
    mdbx_txn_abort(txn);
  if (rc != MDBX_SUCCESS || mdbx_txn_commit(txn) != MDBX_SUCCESS || put_round(env, *dbi, 'A') != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

} // namespace

TEST(issue_gh0104, clone_of_ousted_parked_origin) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_dbi dbi;
  MDBX_env *env = open_env("issue104-clone2.mdbx", &dbi);
  ASSERT_NE(nullptr, env);

  MDBX_txn *origin = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &origin));
  const uint64_t snapshot = mdbx_txn_id(origin);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(origin, false));

  /* write until the parked origin is ousted, so its snapshot is no longer held */
  MDBX_envinfo info;
  for (int round = 0; round < 2000; ++round) {
    ASSERT_EQ(MDBX_SUCCESS, put_round(env, dbi, char('B' + round % 24)));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
    if (info.mi_latter_reader_txnid > snapshot && round > 8)
      break;
  }
  ASSERT_GT(info.mi_latter_reader_txnid, snapshot) << "the writer must have ousted the parked origin";

  MDBX_txn *clone = nullptr;
  EXPECT_EQ(MDBX_OUSTED, mdbx_txn_clone(origin, &clone, nullptr)) << "the snapshot of the ousted origin is recycled";
  EXPECT_EQ(nullptr, clone);
  if (clone)
    mdbx_txn_abort(clone);

  /* the same for a reset handle to reuse, which must stay abortable */
  MDBX_txn *reader = nullptr, *reuse = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_clone(reader, &reuse, nullptr));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_reset(reuse));
  EXPECT_EQ(MDBX_OUSTED, mdbx_txn_clone(origin, &reuse, nullptr));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(reuse));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(reader));

  EXPECT_EQ(MDBX_OUSTED, mdbx_txn_unpark(origin, false));
  mdbx_txn_abort(origin);

  /* the reader table is clean: a write and a new reader work */
  EXPECT_EQ(MDBX_SUCCESS, put_round(env, dbi, 'Z'));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(reader));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  remove_db("issue104-clone2.mdbx");
}

TEST(issue_gh0104, clone_into_foreign_or_origin_handle) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_dbi dbi1, dbi2;
  MDBX_env *env1 = open_env("issue104-clone2a.mdbx", &dbi1);
  ASSERT_NE(nullptr, env1);
  ASSERT_EQ(MDBX_SUCCESS, put_round(env1, dbi1, 'B'));
  MDBX_env *env2 = open_env("issue104-clone2b.mdbx", &dbi2);
  ASSERT_NE(nullptr, env2);

  MDBX_txn *origin = nullptr, *foreign = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env1, nullptr, MDBX_TXN_RDONLY, &origin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env2, nullptr, MDBX_TXN_RDONLY, &foreign));
  const uint64_t foreign_id = mdbx_txn_id(foreign);
  ASSERT_NE(mdbx_txn_id(origin), foreign_id);

  MDBX_txn *target = foreign;
  EXPECT_EQ(MDBX_EINVAL, mdbx_txn_clone(origin, &target, nullptr)) << "a handle of another environment";
  EXPECT_EQ(foreign, target);
  EXPECT_EQ(foreign_id, mdbx_txn_id(foreign)) << "the foreign handle must be left untouched";

  MDBX_txn *finished = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env1, nullptr, MDBX_TXN_RDONLY, &finished));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_reset(finished));
  target = foreign;
  EXPECT_EQ(MDBX_EINVAL, mdbx_txn_clone(finished, &target, nullptr)) << "a handle of another environment";
  EXPECT_EQ(foreign_id, mdbx_txn_id(foreign)) << "the foreign handle must be left untouched, even for a bad source";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(finished));

  target = origin;
  EXPECT_EQ(MDBX_EINVAL, mdbx_txn_clone(origin, &target, nullptr)) << "the origin itself";
  MDBX_val key = {(void *)"k0001", 5}, data;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(origin, dbi1, &key, &data)) << "the origin must stay usable";

  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(foreign));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(origin));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_close(env2));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_close(env1));
  remove_db("issue104-clone2a.mdbx");
  remove_db("issue104-clone2b.mdbx");
}
