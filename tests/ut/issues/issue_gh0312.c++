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
#include "tool_runner.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/312
// Defects of the dump/load/stat tools that lost data or failed while exiting 0:
//  1-2. `mdbx_load -s T` was ignored once a dump header was read, because
//       readhdr() dropped the name at the start of each header; with `-p` the
//       purge then ran on the main table and deleted every named table.
//  3.   `mdbx_load -N` stopped at the first existing key.
//  4.   `mdbx_load -p` purged after restoring the sequence, so the sequence
//       was lost; mdbx_drop() did not reset the sequence of an empty table.
//  5.   `mdbx_load -b` failed when a batch changed nothing.
//  6.   a table whose name is the empty string was restored into @MAIN.
//  7.   a header line longer than 4095 bytes truncated a long table name.
//  8.   `mdbx_dump` wrote `DATA=END` after a table that failed part-way.
//  9.   `mdbx_stat -f` failed on a database with an empty GC.

namespace {

const std::string dump_tool = tool_runner::tool_path("mdbx_dump");
const std::string load_tool = tool_runner::tool_path("mdbx_load");
const std::string stat_tool = tool_runner::tool_path("mdbx_stat");

void remove_file(const std::string &path) {
  std::remove(path.c_str());
  std::remove((path + "-lck").c_str());
}

/* Deletes the test files (databases with their lock files) before the test and on every exit path of it. */
struct files_remover {
  const std::vector<std::string> paths;
  files_remover(std::initializer_list<std::string> list) : paths(list) {
    for (const auto &path : paths)
      remove_file(path);
  }
  ~files_remover() {
    for (const auto &path : paths)
      remove_file(path);
  }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};
using env_ptr = std::unique_ptr<MDBX_env, env_closer>;

struct txn_aborter {
  void operator()(MDBX_txn *txn) const { mdbx_txn_abort(txn); }
};
using txn_ptr = std::unique_ptr<MDBX_txn, txn_aborter>;

bool write_text(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), (std::streamsize)text.size());
  return bool(out);
}

std::string read_text(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/* A plain-text dump section; an empty `table` writes no `database=` line. */
std::string dump_section(const char *table, std::initializer_list<const char *> keys, const char *extra = "") {
  std::string text = "VERSION=3\nformat=print\n";
  if (table)
    text += std::string("database=") + table + "\n";
  text += extra;
  text += "HEADER=END\n";
  for (const char *key : keys)
    text += std::string(" ") + key + "\n v-" + key + "\n";
  return text + "DATA=END\n";
}

int load(std::vector<std::string> args, const std::string &dump, const std::string &db) {
  args.insert(args.begin(), {load_tool, "-q", "-n", "-f", dump});
  args.push_back(db);
  return tool_runner::run(args);
}

/* Opens a database; a positive `pagesize` sets the geometry, for a new one. */
env_ptr open_env(const std::string &db, intptr_t pagesize = -1) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return env_ptr();
  env_ptr owner(env);
  if (mdbx_env_set_maxdbs(env, 8) != MDBX_SUCCESS ||
      (pagesize > 0 && mdbx_env_set_geometry(env, -1, 1 << 20, 256 << 20, 1 << 20, -1, pagesize) != MDBX_SUCCESS) ||
      mdbx_env_open(env, db.c_str(), MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    return env_ptr();
  return owner;
}

/* Number of records in a table (`nullptr` is @MAIN), or -1 if the table does not exist. */
long long entries(const std::string &db, const char *table) {
  env_ptr env = open_env(db);
  MDBX_txn *txn = nullptr;
  if (!env || mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return -2;
  const txn_ptr owner(txn);
  MDBX_dbi dbi = 0;
  MDBX_stat st;
  if (mdbx_dbi_open(txn, table, MDBX_DB_ACCEDE, &dbi) != MDBX_SUCCESS ||
      mdbx_dbi_stat(txn, dbi, &st, sizeof(st)) != MDBX_SUCCESS)
    return -1;
  return (long long)st.ms_entries;
}

uint64_t sequence_of(const std::string &db, const char *table) {
  env_ptr env = open_env(db);
  MDBX_txn *txn = nullptr;
  if (!env || mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return UINT64_MAX;
  const txn_ptr owner(txn);
  MDBX_dbi dbi = 0;
  uint64_t value = UINT64_MAX;
  if (mdbx_dbi_open(txn, table, MDBX_DB_ACCEDE, &dbi) != MDBX_SUCCESS ||
      mdbx_dbi_sequence(txn, dbi, &value, 0) != MDBX_SUCCESS)
    return UINT64_MAX;
  return value;
}

/* Creates a database with one table `name` holding one record, through the API. */
bool create_table(const std::string &db, const char *name, intptr_t pagesize, uint64_t sequence = 0,
                  bool with_record = true) {
  env_ptr env = open_env(db, pagesize);
  MDBX_txn *txn = nullptr;
  if (!env || mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return false;
  txn_ptr owner(txn);
  MDBX_dbi dbi = 0;
  MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
  if (mdbx_dbi_open(txn, name, MDBX_CREATE, &dbi) != MDBX_SUCCESS ||
      (with_record && mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT) != MDBX_SUCCESS) ||
      (sequence && mdbx_dbi_sequence(txn, dbi, nullptr, sequence) != MDBX_SUCCESS))
    return false;
  return mdbx_txn_commit(owner.release()) == MDBX_SUCCESS;
}

} // namespace

TEST(issue_gh0312, load_s_with_purge_keeps_other_tables) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-s.mdbx", ab = "issue312-s-ab.txt", plain = "issue312-s-plain.txt",
                    named = "issue312-s-named.txt";
  const files_remover cleanup{db, ab, plain, named};
  ASSERT_TRUE(write_text(ab, dump_section("A", {"x"}) + dump_section("B", {"y"})));
  ASSERT_TRUE(write_text(plain, dump_section(nullptr, {"k1"})));
  ASSERT_TRUE(write_text(named, dump_section("Other", {"k2", "k3"})));
  ASSERT_EQ(0, load({}, ab, db));

  /* item 1: `-p -s T` must purge T only, not the main table that holds A and B */
  EXPECT_EQ(0, load({"-p", "-s", "T"}, plain, db));
  EXPECT_EQ(1, entries(db, "A")) << "table A must survive `mdbx_load -p -s T`";
  EXPECT_EQ(1, entries(db, "B")) << "table B must survive `mdbx_load -p -s T`";
  EXPECT_EQ(1, entries(db, "T"));

  /* item 2: `-s T` takes priority over the `database=` of the dump */
  EXPECT_EQ(0, load({"-s", "T"}, named, db));
  EXPECT_EQ(3, entries(db, "T"));
  EXPECT_EQ(-1, entries(db, "Other")) << "`-s` must override the table named by the dump";

  /* with `-s` all the sections load into one table, so `-p` must purge it only once */
  EXPECT_EQ(0, load({"-p", "-s", "U"}, ab, db));
  EXPECT_EQ(2, entries(db, "U")) << "`-p -s` must not purge the sections already loaded";
}

/* `-T -s T` loads a headerless dump: the forced table is created even without
   a header, and `-p` purges it rather than the main table that holds A and B. */
TEST(issue_gh0312, load_T_mode_forced_table) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-t.mdbx", ab = "issue312-t-ab.txt", plain = "issue312-t.txt";
  const files_remover cleanup{db, ab, plain};
  ASSERT_TRUE(write_text(ab, dump_section("A", {"x"}) + dump_section("B", {"y"})));
  ASSERT_EQ(0, load({}, ab, db));
  ASSERT_TRUE(write_text(plain, "k1\nv-k1\n"));
  EXPECT_EQ(0, load({"-T", "-p", "-s", "T"}, plain, db));
  EXPECT_EQ(1, entries(db, "T")) << "the forced table must be created even without a header";
  EXPECT_EQ(1, entries(db, "A")) << "`-T -p -s T` must not delete the other tables";
  EXPECT_EQ(1, entries(db, "B"));
}

TEST(issue_gh0312, load_n_skips_existing_keys_and_goes_on) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-n.mdbx", pre = "issue312-n-pre.txt", all = "issue312-n-all.txt";
  const files_remover cleanup{db, pre, all};
  ASSERT_TRUE(write_text(pre, dump_section(nullptr, {"b"})));
  ASSERT_TRUE(write_text(all, dump_section(nullptr, {"a", "b", "c", "d", "e"})));
  ASSERT_EQ(0, load({}, pre, db));
  EXPECT_EQ(0, load({"-N"}, all, db));
  EXPECT_EQ(5, entries(db, nullptr)) << "`mdbx_load -N` must skip the existing key and load the rest";
}

TEST(issue_gh0312, load_r_skips_a_bad_record_and_goes_on) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-r.mdbx", dump = "issue312-r.txt";
  const files_remover cleanup{db, dump};
  /* with 4 KiB pages a 3000-byte key is too long, so `-r` skips it with MDBX_BAD_VALSIZE; `-G` sets the page size
   * on every host (`db_pagesize=` alone is applied only together with `geometry=` or `mapsize=`) */
  const std::string long_key(3000, 'k');
  ASSERT_TRUE(write_text(dump, dump_section(nullptr, {"a", long_key.c_str(), "c"})));
  EXPECT_EQ(0, load({"-r", "-G", "-1:-1:-1:-1:4096"}, dump, db));
  EXPECT_EQ(2, entries(db, nullptr)) << "`mdbx_load -r` must skip the bad record and load the rest";
}

TEST(issue_gh0312, load_p_restores_the_sequence) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-p.mdbx", first = "issue312-p-first.txt", second = "issue312-p-second.txt";
  const files_remover cleanup{db, first, second};
  ASSERT_TRUE(write_text(first, dump_section("S", {"p", "q"})));
  ASSERT_TRUE(write_text(second, dump_section("S", {"x", "y"}, "sequence=100\n")));
  ASSERT_EQ(0, load({}, first, db));
  EXPECT_EQ(0, load({"-p"}, second, db));
  EXPECT_EQ(2, entries(db, "S"));
  EXPECT_EQ(100u, sequence_of(db, "S")) << "`mdbx_load -p` must keep the sequence of the dump";

  /* an empty table with a greater sequence: the purge must reset it too */
  const std::string db2 = "issue312-p2.mdbx";
  const files_remover cleanup2{db2};
  ASSERT_TRUE(create_table(db2, "S", -1, 200, false));
  EXPECT_EQ(0, load({"-p"}, second, db2));
  EXPECT_EQ(100u, sequence_of(db2, "S"));
}

TEST(issue_gh0312, load_b_accepts_an_unchanged_batch) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-b.mdbx", dump = "issue312-b.txt";
  const files_remover cleanup{db, dump};
  ASSERT_TRUE(write_text(dump, dump_section(nullptr, {"a", "b"})));
  ASSERT_EQ(0, load({}, dump, db));
  EXPECT_EQ(0, load({"-b", "1"}, dump, db)) << "a batch that changes nothing is not an error";
  EXPECT_EQ(2, entries(db, nullptr));
}

TEST(issue_gh0312, dump_load_keeps_empty_and_long_table_names) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string empty_src = "issue312-e.mdbx", empty_dst = "issue312-e2.mdbx", empty_dump = "issue312-e.txt",
                    long_src = "issue312-l.mdbx", long_dst = "issue312-l2.mdbx", long_dump = "issue312-l.txt";
  const files_remover cleanup{empty_src, empty_dst, empty_dump, long_src, long_dst, long_dump};

  /* item 6: the table whose name is the empty string */
  ASSERT_TRUE(create_table(empty_src, "", -1));
  ASSERT_EQ(0, tool_runner::run({dump_tool, "-q", "-n", "-a", "-p", "-f", empty_dump, empty_src}));
  EXPECT_EQ(0, load({}, empty_dump, empty_dst));
  EXPECT_EQ(1, entries(empty_dst, "")) << "the table with the empty name must be restored as itself";
  EXPECT_EQ(1, entries(empty_dst, nullptr)) << "@MAIN must hold only the record of that table";

  /* item 7: a 5000-byte table name, longer than the 4096-byte header buffer (64 KiB pages allow it) */
  const std::string name(5000, 'n');
  ASSERT_TRUE(create_table(long_src, name.c_str(), 65536));
  ASSERT_EQ(0, tool_runner::run({dump_tool, "-q", "-n", "-a", "-p", "-f", long_dump, long_src}));
  EXPECT_EQ(0, load({}, long_dump, long_dst));
  EXPECT_EQ(1, entries(long_dst, name.c_str())) << "the long table name must be restored in full";
}

TEST(issue_gh0312, dump_of_a_damaged_table_is_not_complete) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-d.mdbx", dump = "issue312-d.txt", restored = "issue312-d2.mdbx";
  const files_remover cleanup{db, dump, restored};
  {
    env_ptr env = open_env(db, 4096);
    ASSERT_TRUE(env);
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn));
    txn_ptr owner(txn);
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    for (int i = 0; i < 3000; ++i) {
      char key[16];
      snprintf(key, sizeof(key), "k%05d", i);
      MDBX_val k = {key, strlen(key)}, v = {(void *)"valuevaluevaluevalue", 20};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(owner.release()));
  }

  /* Damage the leaf page with the last key: the page header keeps the page flags at byte 10 as a native-endian
   * uint16_t, and 0x02 is a leaf. */
  std::string image = read_text(db);
  const size_t page = 4096, flags_offset = 10;
  size_t damaged = 0;
  for (size_t at = 3 * page; at + page <= image.size(); at += page) {
    uint16_t flags;
    std::memcpy(&flags, &image[at + flags_offset], sizeof(flags));
    if (flags == 0x02 && std::string(image, at, page).find("k02999") != std::string::npos)
      damaged = at;
  }
  ASSERT_NE(0u, damaged) << "no leaf page with the last key";
  const uint16_t bad_flags = 0x7f02;
  std::memcpy(&image[damaged + flags_offset], &bad_flags, sizeof(bad_flags));
  ASSERT_TRUE(write_text(db, image));

  EXPECT_NE(0, tool_runner::run({dump_tool, "-q", "-n", "-p", "-f", dump, db}));
  const std::string text = read_text(dump);
  EXPECT_NE(std::string::npos, text.find("k00000")) << "the records before the damage are dumped";
  EXPECT_EQ(std::string::npos, text.find("DATA=END")) << "a table that failed part-way must not end with DATA=END";
  EXPECT_NE(0, load({}, dump, restored)) << "loading a truncated dump must fail";
}

TEST(issue_gh0312, load_of_a_dump_truncated_inside_a_header_fails) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-h.mdbx";

  { /* the next section's header cut mid-way: `database=Na` without a newline at EOF */
    const std::string dump = "issue312-h1.txt";
    const files_remover cleanup{dump, db};
    ASSERT_TRUE(write_text(dump, dump_section(nullptr, {"a"}) + "VERSION=3\nformat=print\ndatabase=Na"));
    EXPECT_NE(0, load({}, dump, db)) << "a dump that ends inside a table header is truncated";
  }

  { /* EOF right after `VERSION=` of the next section's header */
    const std::string dump = "issue312-h2.txt";
    const files_remover cleanup{dump, db};
    ASSERT_TRUE(write_text(dump, dump_section(nullptr, {"a"}) + "VERSION=3\n"));
    EXPECT_NE(0, load({}, dump, db)) << "a dump that ends right after `VERSION=` of a header is truncated";
  }
}

TEST(issue_gh0312, stat_f_accepts_an_empty_gc) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue312-f.mdbx", dump = "issue312-f.txt", out = "issue312-f.out";
  const files_remover cleanup{db, dump, out};
  ASSERT_TRUE(write_text(dump, dump_section(nullptr, {"a"})));
  ASSERT_EQ(0, load({}, dump, db));
  EXPECT_EQ(0, tool_runner::run({stat_tool, "-n", "-f", db}, nullptr, out.c_str()));
  EXPECT_NE(std::string::npos, read_text(out).find("Total")) << "the page usage must be printed";
}
