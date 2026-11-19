/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "../env-guard.h++"
#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

/* Regression for the finding dupsort-depthmask-shift-unreachable of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * mdbx_dbi_dupsort_depthmask() read the nested-tree height raw from the
 * sub-tree's on-disk record and built the mask as `1u << height`: a crafted
 * database declaring a height beyond the mask width made the shift undefined
 * (in practice the count is truncated and a garbage bit is reported as
 * success). The fix rejects such a height with MDBX_CORRUPTED.
 *
 * The crafting is a three-byte patch of the on-disk sub-tree record
 * (`tree_t.height`, a uint16 right after the uint16 flags at offset +2 of the
 * record): a DUPSORT table with a dupset large enough to be stored as a
 * nested sub-tree carries exactly one such record outside the meta pages
 * (the meta-page tree records have different flags or height, and MAIN has
 * no other tree_t-bearing nodes). */

namespace {

constexpr uint16_t kCraftedHeight = 0xFFFF;
constexpr unsigned kMinSubtreeDepth = 2;

/* Locates the on-disk sub-tree records to craft: the dupset's key ("the-key")
 * is carried by a leaf node whose data is the sub-tree's tree_t (the sub-tree
 * carries the parent's flags stripped of MDBX_DUPSORT, i.e. flags == 0 here),
 * with the uint16 height at +9. The crafted state (height == 2) matches the
 * live record and any COW leftovers of the same content; all of them are
 * patched together. Returns the number of matches. */
unsigned find_subtree_records(uint8_t *file, size_t file_size, size_t meta_region) {
  unsigned matches = 0;
  for (size_t off = meta_region; off + 16 <= file_size; ++off) {
    if (std::memcmp(&file[off], "the-key", 7) != 0)
      continue;
    const uint16_t tflags = static_cast<uint16_t>(file[off + 7] | (file[off + 8] << 8));
    const uint16_t height = static_cast<uint16_t>(file[off + 9] | (file[off + 10] << 8));
    if (tflags == 0 && height >= kMinSubtreeDepth && height < kCraftedHeight) {
      ++matches;
      std::memcpy(&file[off + 9], &kCraftedHeight, 2);
    }
  }
  return matches;
}

} // namespace

TEST(issue_gh0485, depthmask_rejects_crafted_height) {
  const env_files_guard guard({"issue485_depthmask.mdbx"});
  const char *const dbname = "issue485_depthmask.mdbx";

  /* 1. a DUPSORT table with one key whose dupset is stored as a nested
   *    sub-tree (cursor_count_ex depth >= 2), grown page-size-independently */
  MDBX_env *env = nullptr;
  env_handle_guard henv(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_CREATE, &dbi));

  unsigned count = 64;
  unsigned inserted = 0;
  unsigned depth = 0;
  for (; count <= 200000u && depth < kMinSubtreeDepth; count *= 2) {
    for (unsigned i = inserted; i < count; ++i) {
      char key_data[] = "the-key";
      char value_data[16];
      std::snprintf(value_data, sizeof(value_data), "v%014u", i);
      MDBX_val key = {key_data, 7};
      MDBX_val value = {value_data, 15};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_NODUPDATA));
    }
    inserted = count;
    /* the nested sub-tree's depth is the dupset's own, read it through a
     * cursor positioned on the key (the table's depth stays shallow) */
    MDBX_cursor *cur = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cur));
    MDBX_val key = {const_cast<char *>("the-key"), 7};
    MDBX_val value = {nullptr, 0};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &value, MDBX_SET));
    MDBX_stat dstat;
    std::memset(&dstat, 0, sizeof(dstat));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_count_ex(cur, nullptr, &dstat, sizeof(dstat)));
    depth = dstat.ms_depth;
    mdbx_cursor_close(cur);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    txn = nullptr;
    if (depth >= kMinSubtreeDepth)
      break;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  }
  ASSERT_GE(depth, kMinSubtreeDepth) << "the dupset is not a nested sub-tree (last depth " << depth << ")";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  env = nullptr;

  /* 2. locate and patch the sub-tree's tree_t record(s): height = 0xFFFF */
  std::ifstream in(dbname, std::ios::binary);
  ASSERT_TRUE(in.good());
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  ASSERT_GT(file.size(), MDBX_MIN_PAGESIZE * 3);

  MDBX_env *probe = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&probe));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(probe, dbname, MDBX_NOSUBDIR | MDBX_RDONLY, 0444));
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(probe, nullptr, &info, sizeof(info)));
  ASSERT_GE(info.mi_dxb_pagesize, MDBX_MIN_PAGESIZE);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(probe));
  probe = nullptr;

  const unsigned matches = find_subtree_records(file.data(), file.size(), info.mi_dxb_pagesize * 3);
  ASSERT_GE(matches, 1u) << "the sub-tree record is not located";
  {
    std::ofstream out(dbname, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out.write(reinterpret_cast<const char *>(file.data()), (std::streamsize)file.size());
    ASSERT_TRUE(out.good());
  }

  /* 3. the crafted height must be rejected, not shifted */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR | MDBX_RDONLY, 0444));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  /* a fresh dbi handle for the reopened environment */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_DUPSORT, &dbi));
  uint32_t mask = 0;
  EXPECT_EQ(MDBX_CORRUPTED, mdbx_dbi_dupsort_depthmask(txn, dbi, &mask))
      << "a crafted nested-tree height must be rejected (was: undefined shift, garbage mask, success)";
  mdbx_txn_abort(txn);
}
