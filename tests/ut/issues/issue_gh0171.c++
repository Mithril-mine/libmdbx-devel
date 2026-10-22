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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/171
// In copy_with_compacting() the producer hands buffers to a dedicated writer
// thread through a two-slot double buffer guarded by an osal_condpair. When
// osal_write() failed in compacting_write_thread(), the thread set ctx->error
// and jumped to bailout:, which only unlocked the condpair and never signalled
// the part=false condition. A producer already sleeping in
// osal_condpair_wait(&condpair, false) (both buffers in use) waits for exactly
// that signal with an infinite timeout, so it never woke up and never
// re-checked ctx->error: the copy hung forever instead of returning the error.
//
// Reproduced deterministically in a forked child copying to a pipe the parent
// never drains: the writer thread blocks on the full pipe (holding the mutex),
// the producer fills both 1 MiB buffers and goes to sleep in the part=false
// wait; then the parent closes the read end, the blocked writer fails with
// EPIPE and takes the bailout path. Without the fix the child hangs; the
// parent detects that with a bounded waitpid loop and kills the child, so the
// suite never stalls. POSIX-only (fork + pipe).

namespace {

/* Fill the source so the compacted output exceeds two MDBX_ENVCOPY_WRITEBUF
 * (1 MiB) buffers and the producer reaches the head-tail==2 sleep. */
void fill_source(MDBX_env *env, size_t approx_bytes) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return;
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return;
  }
  const size_t vlen = 200;
  std::string value(vlen, 'x');
  char keybuf[24];
  for (size_t i = 0, written = 0; written < approx_bytes; ++i) {
    const int klen = snprintf(keybuf, sizeof keybuf, "k%08zu", i);
    MDBX_val key = {keybuf, size_t(klen)}, data = {&value[0], vlen};
    if (mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) != MDBX_SUCCESS)
      break;
    written += size_t(klen) + vlen;
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
    mdbx_txn_abort(txn);
}

} // namespace

TEST(issue_gh0171, compacting_copy_to_stalled_pipe_does_not_hang) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue171-src.mdbx";
  std::remove(testdb.c_str());
  std::remove((testdb + "-lck").c_str());

  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));
    fill_source(env, 4u << 20); /* ~4 MiB, well over two 1 MiB write buffers */
    mdbx_env_close(env);
  }

  int pipefd[2];
  ASSERT_EQ(0, pipe(pipefd));

  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    /* Child: compact-copy into the pipe. The parent never drains it, so the
     * writer thread blocks inside osal_write() on the full pipe, and the
     * producer fills both buffers and sleeps in the part=false wait. When the
     * parent then closes the read end, the writer fails with EPIPE and takes
     * the bailout path — the deadlock of issue #171 without the fix. */
    close(pipefd[0]);
    MDBX_env *env = nullptr;
    int rc = mdbx_env_create(&env);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS, 0644);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_env_copy2fd(env, pipefd[1], MDBX_CP_COMPACT);
    if (env)
      mdbx_env_close(env);
    close(pipefd[1]);
    /* 2 = failed with EPIPE (the fixed path), 0 = unexpectedly succeeded,
     * 1 = failed with another code. */
    _exit(rc == EPIPE ? 2 : (rc == MDBX_SUCCESS ? 0 : 1));
  }

  /* Parent: keep the read end, let the writer block on the full pipe and the
   * producer sleep, then close the read end to fail the writer. */
  close(pipefd[1]);
  usleep(500000);
  close(pipefd[0]);

  /* Bounded wait: a hang (the bug) is caught here instead of stalling ctest. */
  int status = 0;
  bool exited = false;
  for (int i = 0; i < 150; ++i) { /* up to ~15 s */
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid || (r < 0 && errno == ECHILD)) {
      exited = true;
      break;
    }
    usleep(100000);
  }
  std::remove(testdb.c_str());
  std::remove((testdb + "-lck").c_str());
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    FAIL() << "mdbx_env_copy2fd() hung: the writer thread's bailout did not signal "
              "the producer sleeping in the part=false wait (issue #171)";
  }

  /* The child must have terminated on its own: either returning EPIPE from the
   * copy, or (a rare racing outcome) dying of SIGPIPE on a producer-side write
   * after the read end was closed. Anything else means the fix misbehaves. */
  if (WIFEXITED(status)) {
    EXPECT_EQ(WEXITSTATUS(status), 2) << "the copy must fail with EPIPE and return the error";
  } else {
    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE)
        << "the child must return EPIPE (or die of SIGPIPE), not hang or crash otherwise";
  }
}
