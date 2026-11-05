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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "tool_runner.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/312
// "mdbx_load -p -s T with a dump that has no database= line purges the main
// table and deletes every named table" plus the other mdbx_load / mdbx_dump /
// mdbx_stat contract deviations (items 1-9 of the report; 10-11 are pinned by
// the golden tests and stay report-only).

namespace {

const char *const kDb = "issue312.mdbx";

/* The CTest registration provides MDBX_TOOL_LOAD/DUMP/STAT/CHK with the
   absolute tool paths (see the CMakeLists.txt below), like issue_gh0072. */
static std::string tool_from_env(const char *item, const char *fallback_name) {
  const char *value = getenv(item);
  return (value && *value) ? std::string(value) : tool_runner::tool_path(fallback_name);
}

static const std::string load_tool = tool_from_env("MDBX_TOOL_LOAD", "mdbx_load");
static const std::string dump_tool = tool_from_env("MDBX_TOOL_DUMP", "mdbx_dump");
static const std::string stat_tool = tool_from_env("MDBX_TOOL_STAT", "mdbx_stat");
static const std::string chk_tool = tool_from_env("MDBX_TOOL_CHK", "mdbx_chk");

void remove_db(const char *path) {
  std::remove(path);
  std::remove((std::string(path) + MDBX_LOCK_SUFFIX).c_str());
}

void write_file(const char *name, const std::string &content) {
  std::ofstream f(name, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f.is_open());
  f << content;
}

/* A minimal no-header dump with the given keys. */
std::string plain_dump(const std::vector<const char *> &keys) {
  std::string s = "VERSION=3\nformat=print\nHEADER=END\n";
  for (const char *k : keys) {
    s += std::string(" ") + k + "\n v-" + k + "\n";
  }
  s += "DATA=END\n";
  return s;
}

std::vector<std::string> table_names_of(const std::string &path) {
  const std::string out = path + ".stat.out";
  EXPECT_EQ(tool_runner::run({stat_tool, "-n", "-a", path}, nullptr, out.c_str()), 0);
  std::ifstream f(out);
  std::string line;
  std::vector<std::string> names;
  const std::string prefix = "Status of ";
  while (std::getline(f, line)) {
    if (line.rfind(prefix, 0) == 0)
      names.push_back(line.substr(prefix.size()));
  }
  std::remove(out.c_str());
  return names;
}

bool has_table(const std::vector<std::string> &names, const std::string &name) {
  for (const auto &n : names)
    if (n == name)
      return true;
  return false;
}

size_t entries_of_dump(const std::string &path) {
  const std::string out = path + ".dump.out";
  EXPECT_EQ(tool_runner::run({dump_tool, "-n", "-p", path}, nullptr, out.c_str()), 0);
  std::ifstream f(out);
  std::string line;
  size_t n = 0;
  while (std::getline(f, line)) {
    if (line.rfind(" v-", 0) == 0)
      ++n;
  }
  std::remove(out.c_str());
  return n;
}

size_t entries_of_stat(const std::string &path, const char *table) {
  const std::string out = path + ".stat.out";
  EXPECT_EQ(tool_runner::run({stat_tool, "-n", "-s", table, path}, nullptr, out.c_str()), 0);
  std::ifstream f(out);
  std::string line;
  size_t n = 0;
  const std::string needle = "Entries:";
  while (std::getline(f, line)) {
    const size_t at = line.find(needle);
    if (at != std::string::npos) {
      n = (size_t)strtoull(line.c_str() + at + needle.size(), nullptr, 10);
      break;
    }
  }
  std::remove(out.c_str());
  return n;
}

/* Creates the named table with one record, via the API (item 6/7 setups). */
void make_named_db(const char *path, const char *table_name, unsigned pagesize) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(env, -1, 1 << 20, 256 << 20, 1 << 20, -1, pagesize), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, table_name, (MDBX_db_flags_t)MDBX_CREATE, &dbi), MDBX_SUCCESS);
  MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, (MDBX_put_flags_t)MDBX_UPSERT), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

} // namespace

/* Item 1: `-p -s T` with a dump that has no `database=` line must purge and
   fill the table T, not @MAIN, and must not delete the other tables. */
TEST(issue_gh0312, load_purge_forced_table) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  /* two named tables A and B with one record each */
  write_file("issue312-ab.txt", "VERSION=3\nformat=print\ndatabase=A\nHEADER=END\n x\n y\nDATA=END\n"
                                "VERSION=3\nformat=print\ndatabase=B\nHEADER=END\n x\n y\nDATA=END\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-ab.txt", kDb}), 0);
  const std::vector<std::string> tables = table_names_of(kDb);
  EXPECT_TRUE(has_table(tables, "A"));
  EXPECT_TRUE(has_table(tables, "B"));

  write_file("issue312-t.txt", plain_dump({"k1"}));
  const std::string dump_before = "issue312-before.chk";
  EXPECT_EQ(tool_runner::run({chk_tool, "-q", "-n", kDb}, nullptr, dump_before.c_str()), 0);
  std::remove(dump_before.c_str());

  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-p", "-s", "T", "-f", "issue312-t.txt", kDb}), 0);
  const std::vector<std::string> after = table_names_of(kDb);
  EXPECT_TRUE(has_table(after, "T")) << "the forced table must be created";
  EXPECT_TRUE(has_table(after, "A")) << "-p must purge only the forced table, not the others";
  EXPECT_TRUE(has_table(after, "B")) << "-p must purge only the forced table, not the others";
  EXPECT_EQ(entries_of_stat(kDb, "T"), 1u);
  EXPECT_EQ(tool_runner::run({chk_tool, "-q", "-n", kDb}), 0) << "mdbx_chk must report no error after the purge-load";
  remove_db(kDb);
}

/* Item 2: `-s T` with a dump without `database=` puts the data into T. */
TEST(issue_gh0312, load_forced_table) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-d2.txt", plain_dump({"a", "b"}));
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-s", "T", "-f", "issue312-d2.txt", kDb}), 0);
  EXPECT_EQ(entries_of_stat(kDb, "T"), 2u) << "the data must land in the forced table, not @MAIN";
  remove_db(kDb);
}

/* Item 3: `-N` must skip the existing keys and keep loading the rest. */
TEST(issue_gh0312, load_noooverwrite_continues) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-pre.txt", plain_dump({"b"}));
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-pre.txt", kDb}), 0);
  write_file("issue312-d3.txt", plain_dump({"a", "b", "c", "d", "e"}));
  EXPECT_EQ(tool_runner::run({load_tool, "-q", "-N", "-n", "-f", "issue312-d3.txt", kDb}), 0);
  EXPECT_EQ(entries_of_dump(kDb), 5u) << "-N must skip the existing key and load the rest";
  remove_db(kDb);
}

/* Item 4: `-p` must keep the sequence restored from the dump. */
TEST(issue_gh0312, load_purge_keeps_sequence) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-d4a.txt", "VERSION=3\nformat=print\ndatabase=S\nHEADER=END\n p\n q\nDATA=END\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-d4a.txt", kDb}), 0);
  write_file("issue312-d4.txt", "VERSION=3\nformat=print\ndatabase=S\nsequence=100\nHEADER=END\n x\n y\nDATA=END\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-p", "-f", "issue312-d4.txt", kDb}), 0);
  const std::string out = "issue312-d4.dump";
  ASSERT_EQ(tool_runner::run({dump_tool, "-n", "-p", "-s", "S", kDb}, nullptr, out.c_str()), 0);
  std::ifstream f(out);
  std::string line;
  bool found = false;
  while (std::getline(f, line))
    if (line == "sequence=100")
      found = true;
  std::remove(out.c_str());
  EXPECT_TRUE(found) << "-p must not reset the sequence restored from the dump";
  remove_db(kDb);
}

/* Item 5: `-b N` with a batch that changes nothing must not fail. */
TEST(issue_gh0312, load_batch_no_changes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-d5.txt", plain_dump({"a", "b"}));
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-d5.txt", kDb}), 0);
  EXPECT_EQ(tool_runner::run({load_tool, "-n", "-b", "1", "-f", "issue312-d5.txt", kDb}), 0)
      << "MDBX_RESULT_TRUE of the checkpoint must not be an error";
  remove_db(kDb);
}

/* `-T -p -s T` with a headerless dump: without the header readhdr() is never
   called, so the forced name must still work (and the purge must run on T,
   not @MAIN). */
TEST(issue_gh0312, load_T_mode_forced_table) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  remove_db("issue312-tn.mdbx");
  /* named tables A, B must survive the -T -p -s T load */
  write_file("issue312-tn.txt",
             "VERSION=3\nformat=print\ndatabase=A\nHEADER=END\n x\n y\nDATA=END\n"
             "VERSION=3\nformat=print\ndatabase=B\nHEADER=END\n x\n y\nDATA=END\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-tn.txt", kDb}), 0);

  /* -T mode: a headerless dump — key lines followed by value lines */
  write_file("issue312-nt.txt", "k1\nv-k1\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-T", "-p", "-s", "T", "-f", "issue312-nt.txt", kDb}), 0);
  const std::vector<std::string> tables = table_names_of(kDb);
  EXPECT_TRUE(has_table(tables, "T")) << "the forced table must be created even without a header";
  EXPECT_TRUE(has_table(tables, "A")) << "-T -p -s T must not delete the other tables";
  EXPECT_TRUE(has_table(tables, "B"));
  EXPECT_EQ(entries_of_stat(kDb, "T"), 1u) << "the data must land in the forced table, not @MAIN";
  EXPECT_EQ(tool_runner::run({chk_tool, "-q", "-n", kDb}), 0);
  remove_db(kDb);
  remove_db("issue312-tn.mdbx");
}

/* With `-p -s T` every section of a multi-section dump targets T; the purge
   must run once, so the sections append instead of deleting each other. */
TEST(issue_gh0312, load_purge_forced_table_multi_section) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-ms.txt",
             "VERSION=3\nformat=print\nHEADER=END\n a\n va\nDATA=END\n"
             "VERSION=3\nformat=print\nHEADER=END\n b\n vb\nDATA=END\n"
             "VERSION=3\nformat=print\nHEADER=END\n c\n vc\nDATA=END\n");
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-p", "-s", "T", "-f", "issue312-ms.txt", kDb}), 0);
  EXPECT_EQ(entries_of_stat(kDb, "T"), 3u)
      << "each section must append to the forced table; only the first purge is allowed";
  remove_db(kDb);
}

/* Item 6: a table with an empty name must be restored as the same table. */
TEST(issue_gh0312, empty_table_name_roundtrip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  remove_db("issue312-r6.mdbx");
  make_named_db(kDb, "", 4096);
  ASSERT_EQ(tool_runner::run({dump_tool, "-n", "-a", "-p", kDb}, nullptr, "issue312-d6.txt"), 0);
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-d6.txt", "issue312-r6.mdbx"}), 0);
  const std::vector<std::string> tables = table_names_of("issue312-r6.mdbx");
  EXPECT_TRUE(has_table(tables, "")) << "the empty table name must be restored, not merged into @MAIN";
  remove_db(kDb);
  remove_db("issue312-r6.mdbx");
}

/* Item 7: a 5000-byte table name (64 KiB pages) must survive dump+load. */
TEST(issue_gh0312, long_table_name_roundtrip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  remove_db("issue312-r7.mdbx");
  std::string name(5000, 'n');
  make_named_db(kDb, name.c_str(), 65536);
  ASSERT_EQ(tool_runner::run({dump_tool, "-n", "-a", "-p", kDb}, nullptr, "issue312-d7.txt"), 0);
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-d7.txt", "issue312-r7.mdbx"}), 0);
  const std::vector<std::string> tables = table_names_of("issue312-r7.mdbx");
  EXPECT_EQ(tables.size(), 2u) << "Main DB + the long-named table";
  size_t restored_len = 0;
  for (const auto &n : tables)
    if (n != "Main DB")
      restored_len = n.size();
  EXPECT_EQ(restored_len, 5000u) << "the long table name must not be truncated by the header reader";
  remove_db(kDb);
  remove_db("issue312-r7.mdbx");
}

/* Item 9: `mdbx_stat -f` must not fail on a database with an empty GC. */
TEST(issue_gh0312, stat_f_empty_gc) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT())
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  remove_db(kDb);
  write_file("issue312-d9.txt", plain_dump({"a"}));
  ASSERT_EQ(tool_runner::run({load_tool, "-q", "-n", "-f", "issue312-d9.txt", kDb}), 0);
  EXPECT_EQ(tool_runner::run({stat_tool, "-n", "-f", kDb}), 0) << "an empty GC is not an error";
  remove_db(kDb);
}
