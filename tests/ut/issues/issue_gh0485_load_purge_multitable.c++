/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "../env-guard.h++"
#include "mdbx.h++"
#include "tool_runner.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

/* Regression for the defect load-purge-multitable-trailing-main of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * `mdbx_load -p` ran mdbx_drop() on MAIN for the trailing section of every
 * multi-table dump, but MAIN holds the named tables' descriptors, so the
 * library refused it with MDBX_INCOMPATIBLE after the earlier sections had
 * already committed: every `mdbx_dump -a | mdbx_load -p` of a database with
 * at least one named table exited 1 leaving a partial load. */

static const std::string dump_tool = tool_runner::tool_path("mdbx_dump");
static const std::string load_tool = tool_runner::tool_path("mdbx_load");

static void remove_env(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
  }
  remove(path);
}

static std::string read_file(const char *path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(issue_gh0485, load_purge_multitable_dump) {
  const env_files_guard guard({"issue485_purge.mdbx", "issue485_purge_dst.mdbx"});
  const char *const dbname = "issue485_purge.mdbx";
  const char *const dstname = "issue485_purge_dst.mdbx";
  const char *const dumpname = "issue485_purge.dump";
  const char *const redumpname = "issue485_purge_dst.dump";
  remove_env(dbname);
  remove_env(dstname);
  remove(dumpname);
  remove(redumpname);

  /* a hand-written multi-table dump: named table `alpha` plus raw MAIN
   * records in the trailing section */
  static const char dump[] = "VERSION=3\n"
                             "format=bytevalue\n"
                             "type=btree\n"
                             "database=alpha\n"
                             "HEADER=END\n"
                             " 616b6579\n" /* "akey" */
                             " 6176616c\n" /* "aval" */
                             "DATA=END\n"
                             "HEADER=END\n"
                             " 7261776b6579\n" /* "rawkey" */
                             " 72617776616c\n" /* "rawval" */
                             "DATA=END\n";
  {
    FILE *file = fopen(dumpname, "wb");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(sizeof(dump) - 1, fwrite(dump, 1, sizeof(dump) - 1, file));
    ASSERT_EQ(0, fclose(file));
  }
  /* the issue_gh0058/59/71/72 idiom: an explicit modest upper bound, since
   * the default ~8 GiB geometry cannot be mapped in the ASan/Valgrind cells
   * (check_mmap_limit()) and the too-large-for-RAM upper is runner-dependent.
   * Only the source-creation load carries it: the `load -p` below must apply
   * the geometry line from the dump text, which is part of the contract. */
  ASSERT_EQ(0, tool_runner::run({load_tool, "-G", "-1:1048576:-1:-1:-1", "-f", dumpname, dbname}));

  /* the canonical round trip: dump -a, then load -p into a fresh database
   * (was: exit 1 with MDBX_INCOMPATIBLE on the trailing MAIN section) */
  ASSERT_EQ(0, tool_runner::run({dump_tool, "-a", dbname}, nullptr, dumpname));
  ASSERT_EQ(0, tool_runner::run({load_tool, "-p", "-f", dumpname, dstname}))
      << "`mdbx_load -p` of a multi-table dump must not fail on MAIN";

  /* the loaded copy must be byte-identical when dumped again */
  ASSERT_EQ(0, tool_runner::run({dump_tool, "-a", dstname}, nullptr, redumpname));
  EXPECT_EQ(read_file(dumpname), read_file(redumpname)) << "the purged load must reproduce the source dump";

  remove(redumpname);
  remove(dumpname);
  remove_env(dstname);
  remove_env(dbname);
}
