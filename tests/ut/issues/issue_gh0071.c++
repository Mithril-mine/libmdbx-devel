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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/71
// `mdbx_dump -f <file>` used to truncate the output file during option parsing,
// before the remaining command-line arguments were validated: the `-f` option
// called freopen(path, "w", stdout) inside the getopt loop, so a natural typo
// like `mdbx_dump -f my.db` (where the user meant my.db as the database to
// dump) silently emptied the file before the tool noticed that no database
// path was supplied. The output file must not be touched when the command
// line is invalid.

static const std::string dump_tool = tool_runner::tool_path("mdbx_dump");

static void remove_file(const char *path) {
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
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1));
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

static int run_tool(const std::vector<std::string> &args) { return tool_runner::run(args); }

TEST(issue_gh0071, dump_f_does_not_truncate_on_usage_error) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue71.mdbx";
  const char *const outname = "issue71.dump";
  remove_file(dbname);
  remove_file(outname);
  create_dumpable_db(dbname, 8);

  /* The natural typo: `-f` swallows the database path, leaving none. */
  EXPECT_NE(run_tool({dump_tool, "-f", dbname}), 0) << "a mistyped `-f` invocation must fail";

  /* A correct invocation must still dump the database intact. */
  ASSERT_EQ(run_tool({dump_tool, "-p", "-f", outname, dbname}), 0);

  bool saw_key = false, saw_trailer = false;
  {
    std::ifstream in(outname);
    ASSERT_TRUE(in.good());
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("k0000007") != std::string::npos)
        saw_key = true;
      if (line.rfind("DATA=END", 0) == 0)
        saw_trailer = true;
    }
  }
  EXPECT_TRUE(saw_key) << "the database must contain its data after the failed misuse";
  EXPECT_TRUE(saw_trailer);

  remove_file(outname);
  remove_file(dbname);
}