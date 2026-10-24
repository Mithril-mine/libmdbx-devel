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
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/176
// A nested DUPSORT sub-page stores its node offsets in sp->entries[], but the
// nested loop of page_check() did not validate them against sp->upper, unlike
// the main-page loop (which gained that check with issue #57). Two consumers
// then underflowed on a crafted sub-page:
//   - node_del():       memmove() length = hole_offset - upper, ~SIZE_MAX if
//                       entries[i] < upper;
//   - cursor_put():     memcpy() length = node_ds - upper - PAGEHDRSZ, which
//                       underflows when upper is inflated beyond the page's
//                       usable space -> negative-size-param OOB write
//                       (reported with an AddressSanitizer reproduction,
//                       an ordinary mdbx_put, without MDBX_VALIDATION).
// The tests craft both forms and expect the fixed code to reject them.

namespace {
constexpr size_t PAGEHDRSZ = 20; /* offsetof(page_t, entries) */
constexpr size_t NODESIZE = 8;   /* sizeof(node_t) header without key/data */
constexpr uint16_t P_LEAF = 0x02;
constexpr uint8_t N_DUP = 0x04;

uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

/* A two-dup DUPSORT database: locate the inline leaf sub-page of the key. */
class DupsortDb {
public:
  std::string testdb;
  std::vector<uint8_t> buf;
  size_t node_off = 0;    /* offset of the N_DUP node carrying the sub-page */
  uint32_t sub_dsize = 0; /* node's dsize == size of the sub-page */
  size_t sub_off = 0;     /* offset of the sub-page itself */

  bool create() {
    testdb = "issue176.mdbx";
    try {
      mdbx::env::remove(testdb);
    } catch (const mdbx::error &) {
    }
    mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
    {
      mdbx::txn_managed txn = env.start_write();
      auto map = txn.create_map("", mdbx::key_mode::usual, mdbx::value_mode::multi);
      txn.upsert(map, mdbx::slice("k"), mdbx::slice("v1"));
      txn.upsert(map, mdbx::slice("k"), mdbx::slice("v2"));
      txn.commit();
    }
    env.close();
    return read_file();
  }

  bool read_file() {
    std::fstream f(testdb, std::ios::in | std::ios::binary);
    if (!f.is_open())
      return false;
    f.seekg(0, std::ios::end);
    const std::streamoff end = f.tellg();
    if (end <= 0)
      return false;
    f.seekg(0, std::ios::beg);
    buf.resize(size_t(end));
    f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
    const std::streamsize got = f.gcount();
    if (got != std::streamsize(buf.size()))
      return false;

    /* pagesize: obtain from the environment (platform-dependent). */
    mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
    const uint32_t pagesize = uint32_t(env.get_pagesize());
    env.close();

    for (size_t off = 0; off + pagesize <= buf.size(); off += pagesize) {
      const uint16_t flags = read_u16_le(&buf[off + 10]);
      if (flags != P_LEAF)
        continue;
      const uint16_t lower = read_u16_le(&buf[off + 12]);
      const size_t nkeys = lower / 2;
      for (size_t i = 0; i < nkeys; ++i) {
        const uint16_t entry = read_u16_le(&buf[off + PAGEHDRSZ + 2 * i]);
        const size_t node_position = off + PAGEHDRSZ + entry;
        if (node_position + 8 > off + pagesize)
          continue;
        if (buf[node_position + 4] & N_DUP) {
          const uint32_t dsize = uint32_t(buf[node_position]) | uint32_t(uint32_t(buf[node_position + 1]) << 8) |
                                 uint32_t(uint32_t(buf[node_position + 2]) << 16) |
                                 uint32_t(uint32_t(buf[node_position + 3]) << 24);
          const uint16_t ksize = uint16_t(buf[node_position + 6] | (uint16_t(buf[node_position + 7]) << 8));
          const size_t data_off = node_position + 8 + ksize;
          if (dsize > PAGEHDRSZ && data_off + dsize <= off + pagesize) {
            this->node_off = node_position;
            sub_dsize = dsize;
            sub_off = data_off;
            return true;
          }
        }
      }
    }
    return false;
  }

  void write_file() {
    std::fstream f(testdb, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(0, std::ios::beg);
    f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
    f.close();
  }

  uint16_t upper() const { return read_u16_le(&buf[sub_off + 14]); }
  uint16_t lower() const { return read_u16_le(&buf[sub_off + 12]); }
  void set_entries(size_t i, uint16_t v) { write_u16_le(&buf[sub_off + PAGEHDRSZ + 2 * i], v); }
  void set_upper(uint16_t v) { write_u16_le(&buf[sub_off + 14], v); }
  void set_lower(uint16_t v) { write_u16_le(&buf[sub_off + 12], v); }
  size_t dup_nodes() const { return lower() / 2; }

  /* Plant a fabricated but internally-consistent node header + key starting at
   * sub_off + PAGEHDRSZ + offset (i.e. where entries[offset/2] would alias it
   * through page_node()). This is the same technique the main-page test
   * (issue #57) uses: the node is valid enough to pass every content check
   * except the entries-offset (vs upper) one. */
  void plant_node(size_t offset, const char *key, size_t ksize) {
    const size_t base = sub_off + PAGEHDRSZ + offset;
    std::memset(&buf[base], 0, NODESIZE + ksize);
    write_u16_le(&buf[base + 6], uint16_t(ksize)); /* node->ksize */
    std::memcpy(&buf[base + NODESIZE], key, ksize);
  }
};

template <typename Fn> void expect_rejected(const char *db, bool validation, Fn &&body) {
  auto opts = mdbx::env::operate_parameters(4).set_options(mdbx::env::operate_options().set_exclusive());
  if (validation)
    opts.set_options(mdbx::env::operate_options().set_exclusive().set_enable_validation());
  int rc = 0;
  try {
    mdbx::env_managed env(db, mdbx::env_managed::create_parameters(), opts);
    mdbx::txn_managed txn = env.start_write();
    auto map = txn.open_map("", mdbx::key_mode::usual, mdbx::value_mode::multi);
    body(txn, map);
    txn.commit();
    env.close();
  } catch (const mdbx::exception &e) {
    rc = e.error().code();
  }
  EXPECT_EQ(MDBX_CORRUPTED, rc) << "must be rejected with MDBX_CORRUPTED, got: " << mdbx_strerror(rc);
}

static void cleanup(const std::string &db) {
  try {
    mdbx::env::remove(db);
  } catch (const mdbx::error &) {
  }
}
} // namespace

TEST(issue_gh0176, nested_offset_below_upper_rejected_by_validation) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  DupsortDb db;
  ASSERT_TRUE(db.create()) << "must create a two-dup DUPSORT database";
  ASSERT_NE(0u, db.sub_off) << "N_DUP sub-page not found";
  ASSERT_EQ(2u, db.dup_nodes()) << "must be a two-dup sub-page";
  /* Make the first entry point below upper, into the header/index region, and
   * plant a valid node body exactly there: dsize == 0, flags == 0, ksize == 2
   * and a small key. Every content check but the entries-offset one (mirror
   * of issue #57, added for nested pages) is satisfied. nkeys is shrunk to 1
   * so the aliased second entry is not walked. */
  db.set_lower(2); /* only one entry is walked */
  db.plant_node(0, "a", 2);
  db.write_file();

  expect_rejected(db.testdb.c_str(), true, [](mdbx::txn_managed &txn, mdbx::map_handle map) {
    mdbx::cursor_managed c = txn.open_cursor(map);
    (void)c.find(mdbx::slice("k"), mdbx::cursor::first);
  });
  cleanup(db.testdb);
}

TEST(issue_gh0176, inflated_nested_upper_rejected_on_normal_path) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  DupsortDb db;
  ASSERT_TRUE(db.create()) << "must create a two-dup DUPSORT database";
  ASSERT_NE(0u, db.sub_off) << "N_DUP sub-page not found";
  ASSERT_GT(db.sub_dsize, PAGEHDRSZ + 2);
  db.set_upper(uint16_t(db.sub_dsize - PAGEHDRSZ + 2)); /* upper > usable space */
  db.write_file();

  expect_rejected(db.testdb.c_str(), false, [](mdbx::txn_managed &txn, mdbx::map_handle map) {
    txn.upsert(map, mdbx::slice("k"), mdbx::slice("v3"));
  });
  cleanup(db.testdb);
}

TEST(issue_gh0176, valid_dupsort_database_still_works) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  DupsortDb db;
  ASSERT_TRUE(db.create()) << "must create a two-dup DUPSORT database";
  ASSERT_NE(0u, db.sub_off) << "N_DUP sub-page not found";
  mdbx::env_managed env(db.testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
  {
    mdbx::txn_managed txn = env.start_write();
    auto map = txn.open_map("", mdbx::key_mode::usual, mdbx::value_mode::multi);
    txn.upsert(map, mdbx::slice("k"), mdbx::slice("v3"));
    txn.commit();
  }
  {
    mdbx::txn_managed rtxn = env.start_read();
    auto map = rtxn.open_map("", mdbx::key_mode::usual, mdbx::value_mode::multi);
    mdbx::cursor_managed c = rtxn.open_cursor(map);
    (void)c.find(mdbx::slice("k"));
    EXPECT_EQ(3u, c.count_multivalue()) << "all three dups must be readable";
  }
  env.close();
  cleanup(db.testdb);
}