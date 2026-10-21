/* issue #60 — C++ RAII wrappers:
 *  A) env_managed::close() left handle_ dangling when mdbx_env_close_ex()
 *     released the environment but returned an error (sync/fstat/panic):
 *     the destructor then re-entered the freed object (use-after-free).
 *  B) the *_managed destructors and move-assignments called throwing
 *     close()/abort() from implicitly-noexcept context, so an error there
 *     (e.g. MDBX_THREAD_MISMATCH from mdbx_txn_abort() on a foreign thread)
 *     ended the process with std::terminate instead of being recoverable. */
#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

#include <optional>
#include <thread>

TEST(issue_gh0060, dtor_on_foreign_thread_must_not_terminate) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue60.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  mdbx::env_managed::create_parameters cp;
  mdbx::env_managed::operate_parameters op;
  op.max_maps = 2;
  mdbx::env_managed env(mdbx::path(testdb), cp, op);

  std::optional<mdbx::txn_managed> w;
  w.emplace(env.start_write()); /* write txn begun on the main thread */
  std::puts("write txn started on main thread");

  std::thread t([&] {
    std::puts("destroying txn_managed on a foreign thread...");
    w.reset(); /* ~txn_managed() runs here */
  });
  t.join();

  /* reached here without std::terminate */
  std::puts("RESULT: no terminate on foreign-thread destruction");
}

TEST(issue_gh0060, close_busy_keeps_handle_alive) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue60b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  mdbx::env_managed::create_parameters cp;
  mdbx::env_managed::operate_parameters op;
  op.max_maps = 2;
  mdbx::env_managed env(mdbx::path(testdb), cp, op);

  mdbx::txn_managed txn = env.start_write(); /* owned by the main thread */
  std::thread t([&] {
    /* a close() from a foreign thread fails with MDBX_BUSY; the environment
     * is not released, so handle_ must stay valid (issue #60, defect A) */
    EXPECT_THROW(env.close(), mdbx::exception);
  });
  t.join();

  /* the handle is still live: aborting the txn on its owner thread and
   * closing again must succeed; if close() had cleared handle_ on BUSY, this
   * would call mdbx_env_close_ex(nullptr) -> MDBX_EINVAL and fail the test */
  txn.abort();
  env.close();
  std::puts("RESULT: busy-close kept the handle alive");
}
