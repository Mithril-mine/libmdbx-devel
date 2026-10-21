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

#include "mdbx.h++"
#include "tool_runner.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/70
// A truncated dump was restored as a partial database with `mdbx_load`
// reporting success: EOF in the middle of the data section was treated as a
// normal end because only the terminating `DATA=END` trailer distinguishes a
// complete dump from a cut one, and the trailer was never required.
// Additionally `mdbx_dump` never checked whether its own writes succeeded, so
// a failed write (full disk, broken pipe, ...) was still reported as success.

static const std::string dump_tool = tool_runner::tool_path("mdbx_dump");
static const std::string load_tool = tool_runner::tool_path("mdbx_load");

static void remove_env(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
  }
  remove(path);
}

static void create_dumpable_db(const char *path, size_t count) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  for (size_t i = 0; i < count; ++i) {
    char kbuf[32], vbuf[32];
    int kn = snprintf(kbuf, sizeof kbuf, "k%07zu", i);
    int vn = snprintf(vbuf, sizeof vbuf, "v%07zu", i);
    MDBX_val key = {kbuf, size_t(kn)}, data = {vbuf, size_t(vn)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);
}

static int run_tool(const std::vector<std::string> &args, const char *stdin_file = nullptr,
                    const char *stdout_file = nullptr) {
  return tool_runner::run(args, stdin_file, stdout_file);
}

TEST(issue_gh0070, truncated_dump_is_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue70.mdbx";
  const char *const dumpname = "issue70.dump";
  const char *const truncname = "issue70.trunc.dump";
  const char *const loadname = "issue70.load";
  remove_env(dbname);
  remove(dumpname);
  remove(truncname);
  remove_env(loadname);
  create_dumpable_db(dbname, 8);

  ASSERT_EQ(run_tool({dump_tool, "-p", "-f", dumpname, dbname}), 0);

  /* Cut the dump right after two complete records (key+data lines each). */
  {
    std::ifstream in(dumpname, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string header_tail = "HEADER=END";
    const std::string data(raw.begin(), raw.end());
    const size_t header_end = data.find(header_tail);
    ASSERT_NE(std::string::npos, header_end);
    size_t pos = header_end + header_tail.size();
    /* Skip the rest of the header line (LF, or CRLF on Windows). */
    while (pos < data.size() && data[pos] != '\n')
      pos++;
    if (pos < data.size())
      pos++;
    size_t newlines = 0;
    while (pos < data.size() && newlines < 4) {
      if (data[pos] == '\n')
        newlines++;
      pos++;
    }
    ASSERT_EQ(4u, newlines);
    std::ofstream out(truncname, std::ios::binary);
    out.write(data.data(), (std::streamsize)pos);
  }

  /* The truncated dump must not be reported as a successful restore. */
  EXPECT_NE(run_tool({load_tool, "-f", truncname, loadname}), 0) << "restoring a truncated dump must fail";

  remove_env(loadname);
  remove(truncname);
  remove(dumpname);
  remove_env(dbname);
}

TEST(issue_gh0070, dump_reports_write_errors) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  std::ifstream probe("/dev/full");
  if (!probe.good())
    GTEST_SKIP() << "no /dev/full (non-POSIX host)";

  const char *const dbname = "issue70b.mdbx";
  remove_env(dbname);
  create_dumpable_db(dbname, 8);

  EXPECT_NE(run_tool({dump_tool, dbname}, nullptr, "/dev/full"), 0)
      << "a dump that fails to write must report an error";

  remove_env(dbname);
}