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

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

/* Regression for the defect V-067 of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * fgets() stops at an embedded NUL byte, so the strlen()-based line readers
 * of mdbx_load saw only the prefix of a physical line: a dump line
 * " <NUL>JUNK" loaded as the empty value while the rest of the line was
 * consumed and silently discarded, with the load exiting 0. Any other
 * invalid byte was a format error; NUL was the only silent-swallow case. */

static const std::string load_tool = tool_runner::tool_path("mdbx_load");

static void remove_env(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
  }
  remove(path);
}

static void write_dump(const char *path, const std::vector<char> &content) {
  FILE *file = fopen(path, "wb");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(content.size(), fwrite(content.data(), 1, content.size(), file));
  ASSERT_EQ(0, fclose(file));
}

TEST(issue_gh0485, load_rejects_nul_inside_line) {
  const env_files_guard guard({"issue485_nul.mdbx"});
  const char *const dbname = "issue485_nul.mdbx";
  const char *const dumpname = "issue485_nul.dump";
  remove_env(dbname);
  remove(dumpname);

  /* a valid bytevalue dump with one line carrying an embedded NUL at the
   * value position (line 7): " abc"/" 123", " \0JUNKTAIL", " 555" */
  static const char head[] = "VERSION=3\nformat=bytevalue\ntype=btree\nHEADER=END\n 616263\n 313233\n ";
  static const char tail[] = "JUNKTAIL\n 353535\nDATA=END\n";
  std::vector<char> content;
  content.insert(content.end(), head, head + sizeof(head) - 1);
  content.push_back('\0');
  content.insert(content.end(), tail, tail + sizeof(tail) - 1);
  write_dump(dumpname, content);

  /* the malformed line must be a format error, not a silently truncated
   * record followed by a successful exit */
  const int rc = tool_runner::run({load_tool, "-f", dumpname, dbname});
  EXPECT_NE(0, rc) << "a dump line with an embedded NUL must be rejected";

  remove(dumpname);
  remove_env(dbname);
}
