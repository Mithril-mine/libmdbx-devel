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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "../env-guard.h++"
#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h++"

#if defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L
#include <filesystem>
#endif /* __cpp_lib_filesystem >= 201703L */
#include <thread>

#if defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L

namespace {
const char *kSource = "test-copy-src";
const char *kPlainCopy = "test-copy-plain";
const char *kCompactCopy = "test-copy-compact";

uintmax_t file_size_of(const char *name) {
  std::error_code ec;
  return std::filesystem::file_size(name, ec);
}
} // namespace

TEST(ut_copy_ops, env_copy_plain_and_compact) {
  const env_files_guard guard({"test-copy-compact", "test-copy-plain", "test-copy-src"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
  mdbx::env::remove(mdbx::path(kCompactCopy));

  {
    mdbx::env_managed env(kSource, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    {
      auto txn = env.start_write();
      auto table = txn.create_map("table");
      for (int i = 0; i < 100; ++i)
        txn.upsert(table, mdbx::slice(std::string("key") + std::to_string(i)), mdbx::slice("small-value"));
      /* one large value to exercise the overflow/bigfoot path */
      txn.upsert(table, mdbx::slice("big"), mdbx::slice(std::string(64 * 1024, 'B')));
      txn.commit();
    }
    env.copy(kPlainCopy, /*compactify=*/false);
    env.copy(kCompactCopy, /*compactify=*/true);
  }

  EXPECT_GE(file_size_of(kPlainCopy), 65536u);
  EXPECT_GE(file_size_of(kCompactCopy), 65536u);

  const auto verify = [](const char *path, const char *expect_big) {
    mdbx::env_managed env(path, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_read();
    auto table = txn.open_map("table");
    EXPECT_EQ(txn.get(table, "big"), mdbx::slice(expect_big));
    for (int i = 0; i < 100; ++i)
      EXPECT_EQ(txn.get(table, mdbx::slice(std::string("key") + std::to_string(i))), mdbx::slice("small-value"))
          << "i=" << i;
  };
  verify(kPlainCopy, std::string(64 * 1024, 'B').c_str());
  verify(kCompactCopy, std::string(64 * 1024, 'B').c_str());

  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
  mdbx::env::remove(mdbx::path(kCompactCopy));
}

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int matrix_probe_key = 0;

/* A copy API's matrix: the modes and destinations over the code paths.
 * {plain, compact} x {file, pipe}, the size/throttle/flush flags, the
 * read-txn sources with their dispose/restart semantics, the existing
 * destination's overwrite discipline, and the broken-pipe failure path
 * (the pipe is closed before the copy starts, so the very first write -
 * which the calling thread performs - hits it deterministically, without
 * any timing dependency). Every successful copy is drained/verified by
 * reading the content back, i.e. each mode is checked to traverse and
 * leave consistent its corresponding path: copy_asis/copy_with_compacting,
 * the dedicated writer thread, the meta fixup, the park/unpark throttle
 * and the bailout. */
static void matrix_fill_source(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi table = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "table", MDBX_CREATE, &table));
  for (int i = 0; i < 100; ++i) {
    MDBX_val key = {&i, sizeof(i)};
    MDBX_val value = {&i, sizeof(i)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, table, &key, &value, MDBX_UPSERT));
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

static void matrix_verify_copy(const char *path) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi table = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "table", MDBX_DB_DEFAULTS, &table));
    for (int i = 0; i < 100; ++i) {
      MDBX_val key = {&i, sizeof(i)}, value = {};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_get_ex(txn, table, &key, &value, nullptr)) << "i=" << i;
      ASSERT_EQ(value.iov_len, sizeof(i)) << "i=" << i;
      ASSERT_EQ(*(const int *)value.iov_base, i) << "i=" << i;
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }
  mdbx_env_close(env);
}

static int matrix_drain_to_file(int fd, const char *drained_path) {
  int out = open(drained_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    return -1;
  char buffer[4096];
  ssize_t got;
  while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
    ssize_t done = 0;
    while (done < got) {
      const ssize_t n = write(out, buffer + done, (size_t)(got - done));
      if (n < 0) {
        close(out);
        return -1;
      }
      done += n;
    }
  }
  close(out);
  return (got == 0) ? 0 : -1;
}

TEST(ut_copy_ops, api_modes_matrix) {
  const env_files_guard guard({"test-copy-matrix-src", "test-copy-matrix-src-lck", "test-copy-matrix-dst",
                               "test-copy-matrix-dst-lck", "test-copy-matrix-drained"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *dst = "test-copy-matrix-dst";
  const char *drained = "test-copy-matrix-drained";
  mdbx::env::remove(mdbx::path("test-copy-matrix-src"));

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, "test-copy-matrix-src", MDBX_NOSUBDIR, 0644));
  matrix_fill_source(env);

  const int modes[] = {MDBX_CP_DEFAULTS, MDBX_CP_COMPACT};
  for (const int mode : modes) {
    /* file destination: copy_asis/copy_with_compacting + the stub-meta,
     * flush and actual-meta writes of the pathname path */
    SCOPED_TRACE(::testing::Message() << "file destination, mode " << mode);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_copy(env, dst, (MDBX_copy_flags_t)(mode | MDBX_CP_OVERWRITE)));
    matrix_verify_copy(dst);

    /* pipe destination: the fd path, with the copy drained into a file by
     * a concurrent reader - the drained bytes must form a valid database */
    SCOPED_TRACE(::testing::Message() << "pipe destination, mode " << mode);
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));
    std::thread drain([&] { ASSERT_EQ(0, matrix_drain_to_file(pipefd[0], drained)); });
    EXPECT_EQ(MDBX_SUCCESS, mdbx_env_copy2fd(env, pipefd[1], (MDBX_copy_flags_t)mode));
    close(pipefd[1]);
    drain.join();
    close(pipefd[0]);
    matrix_verify_copy(drained);
  }

  /* COMPACT + FORCE_DYNAMIC_SIZE: the compacted copy's dynamic geometry */
  SCOPED_TRACE("file destination, COMPACT + FORCE_DYNAMIC_SIZE");
  EXPECT_EQ(
      MDBX_SUCCESS,
      mdbx_env_copy(env, dst, (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_FORCE_DYNAMIC_SIZE | MDBX_CP_OVERWRITE)));
  matrix_verify_copy(dst);

  /* COMPACT + DONT_FLUSH: the flush-skipping branch of the fd/path path */
  SCOPED_TRACE("file destination, COMPACT + DONT_FLUSH");
  EXPECT_EQ(MDBX_SUCCESS,
            mdbx_env_copy(env, dst, (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_DONT_FLUSH | MDBX_CP_OVERWRITE)));
  matrix_verify_copy(dst);

  /* COMPACT + THROTTLE_MVCC into a drained pipe: the park/unpark throttle */
  SCOPED_TRACE("pipe destination, COMPACT + THROTTLE_MVCC");
  {
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));
    std::thread drain([&] { ASSERT_EQ(0, matrix_drain_to_file(pipefd[0], drained)); });
    EXPECT_EQ(MDBX_SUCCESS,
              mdbx_env_copy2fd(env, pipefd[1], (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_THROTTLE_MVCC)));
    close(pipefd[1]);
    drain.join();
    close(pipefd[0]);
    matrix_verify_copy(drained);
  }

  /* A read transaction as the source: the txn_ro paths, with the MVCC
   * throttling and the dispose/restart semantics of the txn-based copies. */
  {
    MDBX_txn *snapshot = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &snapshot));
    MDBX_dbi table = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(snapshot, "table", MDBX_DB_DEFAULTS, &table));
    MDBX_val key = {&matrix_probe_key, sizeof(matrix_probe_key)}, value = {};

    SCOPED_TRACE("txn source, file destination, COMPACT + THROTTLE_MVCC");
    EXPECT_EQ(MDBX_SUCCESS,
              mdbx_txn_copy2pathname(snapshot, dst,
                                     (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_THROTTLE_MVCC | MDBX_CP_OVERWRITE)));
    matrix_verify_copy(dst);
    /* without DISPOSE_TXN the snapshot stays usable */
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get_ex(snapshot, table, &key, &value, nullptr));

    SCOPED_TRACE("txn source, pipe destination, COMPACT + THROTTLE_MVCC + DISPOSE_TXN");
    {
      int pipefd[2];
      ASSERT_EQ(0, pipe(pipefd));
      std::thread drain([&] { ASSERT_EQ(0, matrix_drain_to_file(pipefd[0], drained)); });
      EXPECT_EQ(MDBX_SUCCESS,
                mdbx_txn_copy2fd(snapshot, pipefd[1],
                                 (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_THROTTLE_MVCC | MDBX_CP_DISPOSE_TXN)));
      close(pipefd[1]);
      drain.join();
      close(pipefd[0]);
      matrix_verify_copy(drained);
    }
    /* DISPOSE_TXN aborts/disposes the passed transaction: the handle is
     * finished by the flag's contract, so it is deliberately not used
     * (not renewed, not aborted) any further here. */
  }

  /* the existing destination's discipline: a refusal without MDBX_CP_OVERWRITE
   * and a successful replacement with it */
  SCOPED_TRACE("existing destination, COMPACT without/with OVERWRITE");
  EXPECT_NE(MDBX_SUCCESS, mdbx_env_copy(env, dst, (MDBX_copy_flags_t)MDBX_CP_COMPACT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_copy(env, dst, (MDBX_copy_flags_t)(MDBX_CP_COMPACT | MDBX_CP_OVERWRITE)));
  matrix_verify_copy(dst);

  /* the broken-pipe failure path, for both the compacting (the dedicated
   * writer thread) and the plain (the caller's writes only) modes: the copy
   * must fail with EPIPE, not kill the process by the SIGPIPE */
  for (const int mode : modes) {
    SCOPED_TRACE(::testing::Message() << "closed pipe destination, mode " << mode);
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));
    close(pipefd[0]); /* the reader goes away before the copy starts */
    EXPECT_EQ(EPIPE, mdbx_env_copy2fd(env, pipefd[1], (MDBX_copy_flags_t)mode))
        << "the copy must fail with EPIPE, not die by SIGPIPE";
    close(pipefd[1]);
  }

  /* the environment must stay fully usable after all the paths above */
  matrix_fill_source(env);
  mdbx_env_close(env);
  mdbx::env::remove(mdbx::path("test-copy-matrix-src"));
  mdbx::env::remove(mdbx::path(dst));
}
#endif /* !_WIN32 */

TEST(ut_copy_ops, txn_copy_snapshot) {
  const env_files_guard guard({"test-copy-compact", "test-copy-plain", "test-copy-src"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));

  mdbx::env_managed env(kSource, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table");
    for (int i = 0; i < 50; ++i)
      txn.upsert(table, mdbx::slice(std::string("key") + std::to_string(i)), mdbx::slice("v"));
    txn.commit();
  }
  {
    /* snapshot copy taken from a read txn while a concurrent writer proceeds */
    auto snapshot = env.start_read();
    auto table = snapshot.open_map("table");
    std::thread writer([&] {
      auto w = env.start_write();
      w.upsert(w.create_map("later"), "k", "x");
      w.commit();
    });
    EXPECT_EQ(snapshot.get(table, "key0"), mdbx::slice("v"));
    snapshot.copy(kPlainCopy, /*compactify=*/false);
    writer.join();
  }
  /* On Windows a memory-mapped environment cannot be deleted while it is
   * still open (decl-env.h++ remove_mode notes), so close before removing. */
  env.close();

  /* the copy contains the snapshot state, but not the later table */
  {
    mdbx::env_managed copy(kPlainCopy, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = copy.start_read();
    EXPECT_EQ(txn.get(txn.open_map("table"), "key0"), mdbx::slice("v"));
    EXPECT_THROW((void)txn.open_map("later"), mdbx::not_found) << "later table must not leak into the snapshot";
  }

  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
}

#else /* !(filesystem available) */

TEST(ut_copy_ops, env_copy_plain_and_compact) {
  const env_files_guard guard({"test-copy-compact", "test-copy-plain", "test-copy-src"});
  GTEST_SKIP() << "requires <filesystem> (C++17), disabled since the requested standard is older";
}

TEST(ut_copy_ops, txn_copy_snapshot) {
  const env_files_guard guard({"test-copy-compact", "test-copy-plain", "test-copy-src"});
  GTEST_SKIP() << "requires <filesystem> (C++17), disabled since the requested standard is older";
}

#endif /* __cpp_lib_filesystem >= 201703L */