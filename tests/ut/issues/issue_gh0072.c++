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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/72
// A table name may contain any byte except NUL, so a name containing a newline
// made `mdbx_dump` emit a second `database=` header line (routing the restore
// into a different table) and leaked control/escape sequences into the
// terminal through `mdbx_dump -l` and `mdbx_stat -a`. Dumping such a table is
// now refused, listing/stat print a sanitized name, and `mdbx_load` rejects a
// `database=` header value that contains control characters.

static const std::string dump_tool = tool_runner::tool_path("mdbx_dump");
static const std::string load_tool = tool_runner::tool_path("mdbx_load");
static const std::string stat_tool = tool_runner::tool_path("mdbx_stat");

static void remove_env(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
  }
  remove(path);
}

/* Creates a database whose named table contains a newline byte. */
static void create_hostile_db(const char *path) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_dbi sub = 0;
  const char hostile_name[] = {'x', '\n', 'y', 0};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, hostile_name, MDBX_CREATE, &sub));
  MDBX_val key = {(void *)"k1", 2}, data = {(void *)"v1", 2};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, sub, &key, &data, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);
}

static int run_tool(const std::vector<std::string> &args, const char *stdout_file = nullptr) {
  return tool_runner::run(args, nullptr, stdout_file);
}

TEST(issue_gh0072, dump_refuses_control_char_table_name) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue72.mdbx";
  remove_env(dbname);
  create_hostile_db(dbname);

  EXPECT_NE(run_tool({dump_tool, "-a", dbname}), 0) << "dumping a table whose name breaks the header format must fail";

  remove_env(dbname);
}

TEST(issue_gh0072, dump_list_sanitizes_control_chars) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue72-list.mdbx";
  const char *const outname = "issue72-list.txt";
  remove_env(dbname);
  remove(outname);
  create_hostile_db(dbname);

  ASSERT_EQ(run_tool({dump_tool, "-l", "-f", outname, dbname}), 0);

  {
    std::ifstream in(outname);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, text.find("x?y")) << "the control characters must be sanitized";
  }

  remove(outname);
  remove_env(dbname);
}

TEST(issue_gh0072, stat_sanitizes_control_chars) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue72-stat.mdbx";
  const char *const outname = "issue72-stat.txt";
  remove_env(dbname);
  remove(outname);
  create_hostile_db(dbname);

  ASSERT_EQ(run_tool({stat_tool, "-a", dbname}, outname), 0);

  {
    std::ifstream in(outname);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, text.find("Status of x?y")) << "the control characters must be sanitized";
  }

  remove(outname);
  remove_env(dbname);
}

TEST(issue_gh0072, load_rejects_control_char_name) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dumpname = "issue72.dump";
  const char *const loadname = "issue72.load";
  remove_env(loadname);
  remove(dumpname);

  /* A `database=` header value containing a control character (a tab here)
   * must be rejected instead of being used to select the target table. */
  const char *const dump_text = "VERSION=3\n"
                                "format=print\n"
                                "type=btree\n"
                                "duplicates=0\n"
                                "database=evil\tb\n"
                                "HEADER=END\n"
                                " k1\n"
                                " v1\n"
                                "DATA=END\n";
  {
    std::ofstream out(dumpname, std::ios::binary);
    const size_t len = strlen(dump_text);
    out.write(dump_text, (std::streamsize)len);
  }

  EXPECT_NE(run_tool({load_tool, "-f", dumpname, loadname}), 0)
      << "a header with a control-character name must be rejected";

  remove(dumpname);
  remove_env(loadname);
}