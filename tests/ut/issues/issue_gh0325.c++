/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/325
///
/// mvcc_snapshot_oldest_rw() rescanned the whole reader table until one full
/// pass completed with no reader starting or ending a transaction. Under
/// heavy reader churn every pass was invalidated, so the writer retried
/// unboundedly: the oldest-reader scan took milliseconds per call (up to
/// tens of milliseconds) and writer throughput collapsed.
///
/// Now the hot path (txn_gc_detent()) bounds the scan: after a few unstable
/// passes it returns the previous oldest (a safe lower bound, only not the
/// newest one), leaves the cache and the refresh flag untouched, and the
/// next call scans again. The exact-scan callers (kick-laggards, gc-info,
/// the forced rescan before the file grows) are unchanged.
///
/// Requires a probes-enabled build: the test counts the scan passes of a
/// single call via the mvcc_oldest_scan_pass watch site, whose recorded
/// value is the ordinal number of the last executed pass.

#include "mdbx.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../probe-ctl.h"
#include "../test-env.h"

namespace {

struct files_remover {
  const std::vector<std::string> paths;
  explicit files_remover(std::initializer_list<std::string> list) : paths(list) {
    for (const auto &path : paths)
      std::remove(path.c_str());
  }
  ~files_remover() {
    for (const auto &path : paths)
      std::remove(path.c_str());
  }
};

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

/* читает из reply "query" поле value (последнее наблюдённое значение WATCH) */
int probe_value(const char *reply) {
  const char *line = std::strstr(reply, "site mvcc_oldest_scan_pass ");
  if (!line)
    return -1;
  /* site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file:line> */
  int fields[6] = {0, 0, 0, 0, 0, 0};
  const char *p = line + strlen("site mvcc_oldest_scan_pass ");
  for (int i = 0; i < 6; ++i) {
    char *end = nullptr;
    fields[i] = (int)std::strtol(p, &end, 10);
    if (end == p)
      return -1;
    p = end;
  }
  return fields[5];
}

TEST(issue_gh0325, oldest_reader_scan_is_bounded_under_reader_churn) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0325.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxreaders(env.get(), 64), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644), MDBX_SUCCESS);

  MDBX_dbi dbi = 0;
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }

  /* the churn: threads restarting a read transaction in a tight loop */
  std::atomic<bool> stop{false};
  std::atomic<long> churn_ops{0};
  auto churner = [&]() {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
      return;
    long ops = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (mdbx_txn_reset(txn) != MDBX_SUCCESS || mdbx_txn_renew(txn) != MDBX_SUCCESS)
        break;
      ++ops;
    }
    mdbx_txn_abort(txn);
    churn_ops.store(ops, std::memory_order_relaxed);
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i)
    threads.emplace_back(churner);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  /* прогрев: несколько циклов writer'а устанавливают cached_oldest */
  for (int i = 0; i < 16; ++i) {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_txn_info info;
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(mdbx_txn_info(txn, &info, true), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }

  /* инвариант: один вызов txn_info() при активном churn делает не более
   * четырёх проходов по таблице читателей */
  char reply[512];
  for (int i = 0; i < 16; ++i) {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    ASSERT_EQ(0, mdbx_probe_ctl("reset mvcc_oldest_scan_pass", reply, sizeof reply));
    MDBX_txn_info info;
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(mdbx_txn_info(txn, &info, true), MDBX_SUCCESS);
    ASSERT_EQ(0, mdbx_probe_ctl("query mvcc_oldest_scan_pass", reply, sizeof reply));
    const int passes = probe_value(reply);
    ASSERT_GE(passes, 1) << "the scan must run at least one pass";
    ASSERT_LE(passes, 4) << "the scan must bail out after a few unstable passes, reply: " << reply;
    ASSERT_GE(info.txn_reader_lag, 0);
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }

  stop.store(true);
  for (auto &t : threads)
    t.join();
  EXPECT_GT(churn_ops.load(), 0) << "the churn threads must actually restart read transactions";
}

} /* namespace */
