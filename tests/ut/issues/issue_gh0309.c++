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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "../env-guard.h++"
#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/309
// A compacting copy to a pipe (mdbx_env_copy2fd() with MDBX_CP_COMPACT on a
// non-seekable fd) writes the meta before the tree walk, because a pipe cannot
// be rewritten afterwards. The meta still named the source root: the walk
// renumbers the pages and computes the new root only in memory, so the copy
// named an arbitrary page as its root. The copy returned MDBX_SUCCESS and most
// records of the backup were unreachable. POSIX-only (pipe).

namespace {

const char *const kSource = "issue309-src.mdbx";
const char *const kCopy = "issue309-copy.mdbx";
const int kKeys = 3000;
const char kValue[] = "valuevaluevaluevaluevaluevaluevaluevaluevaluevalue";

void remove_db(const char *path) {
  std::remove(path);
  std::remove((std::string(path) + "-lck").c_str());
}

/* Deletes both databases before the test and on every exit path of it. */
struct db_remover {
  db_remover() { remove_db(kSource), remove_db(kCopy); }
  ~db_remover() { remove_db(kSource), remove_db(kCopy); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

struct txn_aborter {
  void operator()(MDBX_txn *txn) const { mdbx_txn_abort(txn); }
};

struct cursor_closer {
  void operator()(MDBX_cursor *cursor) const { mdbx_cursor_close(cursor); }
};

std::string key_of(int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "k%08d", i);
  return buf;
}

} // namespace

TEST(issue_gh0309, compacting_copy_to_pipe_keeps_all_records) {
  const env_files_guard guard({"issue309-src.mdbx", "issue309-copy.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, 1 << 20, 256 << 20, 1 << 20, -1, 4096));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kSource, MDBX_NOSUBDIR, 0644));

  /* Many small commits that rewrite the keys in a scattered order, so the source root moves away from the
   * page the walk writes last. */
  for (int round = 0; round < 40; ++round) {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(txn);
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    for (int i = 0; i < 400; ++i) {
      std::string k = key_of((i * 7919 + round) % kKeys);
      MDBX_val key = {&k[0], k.size()}, data = {(void *)kValue, sizeof(kValue) - 1};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn_owner.release()));
  }

  /* The compacting copy goes into a pipe; a second thread drains it, as `mdbx_copy -c src | cat` does. */
  int pipefd[2];
  ASSERT_EQ(0, pipe(pipefd));
  std::string image;
  std::thread drainer([&image, fd = pipefd[0]] {
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
      if (n > 0)
        image.append(buf, size_t(n));
  });
  const int copy_rc = mdbx_env_copy2fd(env, pipefd[1], MDBX_CP_COMPACT);
  close(pipefd[1]);
  drainer.join();
  close(pipefd[0]);
  env_owner.reset();
  ASSERT_EQ(MDBX_SUCCESS, copy_rc);

  FILE *const out = std::fopen(kCopy, "wb");
  ASSERT_NE(nullptr, out);
  const size_t written = std::fwrite(image.data(), 1, image.size(), out);
  ASSERT_EQ(0, std::fclose(out));
  ASSERT_EQ(image.size(), written);

  /* Every record must be reachable in the copy, in key order. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  env_owner.reset(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kCopy, MDBX_NOSUBDIR, 0644));
  MDBX_txn *reader = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  const std::unique_ptr<MDBX_txn, txn_aborter> reader_owner(reader);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(reader, nullptr, MDBX_DB_DEFAULTS, &dbi));
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(reader, dbi, &cursor));
  const std::unique_ptr<MDBX_cursor, cursor_closer> cursor_owner(cursor);
  MDBX_val key, data;
  int count = 0, rc;
  while ((rc = mdbx_cursor_get(cursor, &key, &data, count ? MDBX_NEXT : MDBX_FIRST)) == MDBX_SUCCESS) {
    ASSERT_EQ(key_of(count), std::string((const char *)key.iov_base, key.iov_len)) << "record " << count;
    ASSERT_EQ(std::string(kValue), std::string((const char *)data.iov_base, data.iov_len)) << "record " << count;
    ++count;
  }
  EXPECT_EQ(MDBX_NOTFOUND, rc);
  EXPECT_EQ(kKeys, count) << "records reachable in the compacted copy";
}
