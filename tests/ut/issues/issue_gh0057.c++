#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/57
// A non-DUPFIX node offset below page->upper passed page_check(), so a crafted
// page was admitted even under MDBX_VALIDATION; node_del()/node_shrink() then
// computed a memmove() length of ~2^64 from the signed underflow, an OOB write.
// The test crafts a single-node leaf whose entries[0] points into the header
// area (offset 0 < upper), with a fabricated node header that keeps the old
// page_check() satisfied (small ksize, no duplicate-order to compare against),
// then reopens the database with MDBX_VALIDATION and expects the first data
// access to be rejected with MDBX_CORRUPTED.
static uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

TEST(issue_gh0057, node_offset_below_upper_rejected) {
  const std::string testdb = "issue57.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, pagesize));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_val key = {const_cast<char *>("k"), 1};
  MDBX_val data = {const_cast<char *>("v"), 1};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);

  // Read the whole file and locate the single-node leaf page.
  std::fstream f(testdb, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(f.is_open());
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  ASSERT_GT(end, 0);
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  ASSERT_EQ(f.gcount(), std::streamsize(buf.size()));

  constexpr size_t PAGEHDRSZ = 20;  /* offsetof(page_t, entries), non-dupfix pages */
  constexpr uint16_t P_LEAF = 0x02; /* page_type enum value */
  bool patched = false;
  for (size_t off = 0; off + pagesize <= buf.size(); off += pagesize) {
    const uint16_t flags = read_u16_le(&buf[off + 10]);
    const uint16_t lower = read_u16_le(&buf[off + 12]);
    const uint16_t upper = read_u16_le(&buf[off + 14]);
    /* Note: the on-disk `lower` is the index size (nkeys * sizeof(indx_t))
     * without the page header, so a single-node leaf has lower == 2. */
    if (flags == P_LEAF && lower == 2 && upper > PAGEHDRSZ) {
      /* entries[0] must point below upper; plant a node header that keeps the
       * old page_check() satisfied: ksize=4, a 4-byte key, no ordering peer. */
      write_u16_le(&buf[off + PAGEHDRSZ + 0], 0);        /* entries[0] = 0 < upper */
      write_u16_le(&buf[off + PAGEHDRSZ + 6], 4);        /* fabricated node->ksize */
      std::memcpy(&buf[off + PAGEHDRSZ + 8], "junk", 4); /* fabricated key */
      patched = true;
      break;
    }
  }
  ASSERT_TRUE(patched) << "single-node leaf page not found in the database file";

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  ASSERT_TRUE(f.good());
  f.close();

  // Reopen with MDBX_VALIDATION: the crafted page must be rejected on load.
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR | MDBX_VALIDATION, 0));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_val out = {};
  const int rc = mdbx_get(txn, dbi, &key, &out);
  EXPECT_EQ(MDBX_CORRUPTED, rc);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}
