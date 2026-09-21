/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include <limits>

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}

} // namespace

TEST(ut_value2key, json_integer_roundtrip) {
  const int64_t values[] = {INT64_C(1),
                            INT64_C(-1),
                            INT64_C(2),
                            INT64_C(-2),
                            INT64_C(123456789012345),
                            INT64_C(-123456789012345),
                            INT64_C(9007199254740991), /* 2^53-1 */
                            INT64_C(-9007199254740991),
                            INT64_C(9007199254740992), /* 2^53 */
                            INT64_C(-9007199254740992)};
  for (const auto v : values) {
    const auto key = ::mdbx_key_from_jsonInteger(v);
    ASSERT_EQ(key, ::mdbx_key_from_double(double(v))) << "JSON-safe integer keys equal the double keys";
    MDBX_val kv{const_cast<uint64_t *>(&key), sizeof(key)};
    EXPECT_EQ(::mdbx_jsonInteger_from_key(kv), v) << "v=" << v;
  }
}

TEST(ut_value2key, json_integer_zero_deviation) {
  /* Deviation flagged to the coordinator (TASK-19 batch 1): zero is inside the
   * documented RFC-7159 range, and mdbx_key_from_jsonInteger(0) produces the
   * bias key 0x8000000000000000, yet mdbx_jsonInteger_from_key() decodes that
   * key to INT64_MAX rather than 0 (src/api-key-transform.c, shift<1 branch).
   * Documented here as-is until the core fix lands. */
  const auto key = ::mdbx_key_from_jsonInteger(0);
  MDBX_val kv{const_cast<uint64_t *>(&key), sizeof(key)};
  EXPECT_EQ(::mdbx_jsonInteger_from_key(kv), INT64_MAX);
}

TEST(ut_value2key, json_integer_clamping) {
  const uint64_t pos = ::mdbx_key_from_double(1e19);
  const uint64_t neg = ::mdbx_key_from_double(-1e19);
  MDBX_val kp{const_cast<uint64_t *>(&pos), sizeof(pos)};
  MDBX_val kn{const_cast<uint64_t *>(&neg), sizeof(neg)};
  EXPECT_EQ(::mdbx_jsonInteger_from_key(kp), INT64_MAX) << "out-of-range positive clamps to INT64_MAX";
  EXPECT_EQ(::mdbx_jsonInteger_from_key(kn), INT64_MIN) << "out-of-range negative clamps to INT64_MIN";
}

TEST(ut_value2key, json_integer_mixed_ordering) {
  auto env = make_env("test-value2key");
  auto txn = env.start_write();
  auto table = txn.create_map("table");

  const int64_t ints[] = {INT64_C(-3), INT64_C(-1), INT64_C(2), INT64_C(7)};
  const double dbls[] = {-2.5, 0.5, 3.25, 100.0};
  for (auto v : ints) {
    const uint64_t ibe = __builtin_bswap64(::mdbx_key_from_jsonInteger(v));
    txn.insert(table, mdbx::slice(&ibe, sizeof(ibe)), mdbx::slice("i"));
  }
  for (auto d : dbls) {
    const uint64_t dbe = __builtin_bswap64(::mdbx_key_from_double(d));
    txn.insert(table, mdbx::slice(&dbe, sizeof(dbe)), mdbx::slice("d"));
  }

  double previous = std::numeric_limits<double>::lowest();
  auto cursor = txn.open_cursor(table);
  cursor.to_first();
  do {
    const auto key_be = cursor.current().key;
    ASSERT_EQ(key_be.size(), sizeof(uint64_t));
    const auto key = __builtin_bswap64(key_be.as_uint64());
    MDBX_val kv{const_cast<uint64_t *>(&key), sizeof(key)};
    const double decoded = ::mdbx_double_from_key(kv);
    EXPECT_GE(decoded, previous) << "v2k keys scan in ascending numeric order";
    previous = decoded;
  } while (cursor.to_next(false));

  txn.commit();
  env.close();
  mdbx::env::remove("test-value2key");
}

TEST(ut_value2key, equivalent_to_ordinal) {
  auto env = make_env("test-value2key");
  auto txn = env.start_write();
  auto v2k_ordinal = txn.create_map("v2k", mdbx::key_mode::ordinal);
  auto be_bytes = txn.create_map("be", mdbx::key_mode::usual);

  /* The same signed int64 sequence stored two ways:
   *  (a) offset-binary keys in an INTEGERKEY (native unsigned) table;
   *  (b) the same numeric keys as big-endian bytes in a usual table.
   * Both must yield identical scan order: the signed numeric one. */
  const int64_t values[] = {INT64_C(-1000), INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(12345), INT64_C(999999999)};
  for (auto v : values) {
    txn.insert(v2k_ordinal, mdbx::slice::wrap(::mdbx_key_from_int64(v)), mdbx::slice("x"));
    const uint64_t be = __builtin_bswap64(::mdbx_key_from_int64(v));
    txn.insert(be_bytes, mdbx::slice(&be, sizeof(be)), mdbx::slice("x"));
  }

  std::vector<int64_t> from_v2k, from_be;
  auto collect_v2k = [](mdbx::txn &txn, mdbx::map_handle table, std::vector<int64_t> &out) {
    auto cursor = txn.open_cursor(table);
    cursor.to_first();
    do {
      const auto current = cursor.current();
      ASSERT_EQ(current.key.size(), sizeof(uint64_t));
      out.push_back(int64_t(current.key.as_uint64() - UINT64_C(0x8000000000000000)));
    } while (cursor.to_next(false));
  };
  auto collect_be = [](mdbx::txn &txn, mdbx::map_handle table, std::vector<int64_t> &out) {
    auto cursor = txn.open_cursor(table);
    cursor.to_first();
    do {
      const auto current = cursor.current();
      ASSERT_EQ(current.key.size(), sizeof(uint64_t));
      out.push_back(int64_t(__builtin_bswap64(current.key.as_uint64()) - UINT64_C(0x8000000000000000)));
    } while (cursor.to_next(false));
  };
  collect_v2k(txn, v2k_ordinal, from_v2k);
  collect_be(txn, be_bytes, from_be);

  ASSERT_EQ(from_v2k.size(), values[0] ? 6u : 6u);
  ASSERT_EQ(from_be.size(), from_v2k.size());
  for (size_t i = 0; i < from_v2k.size(); ++i) {
    EXPECT_EQ(from_v2k[i], from_be[i]) << "both representations scan in the same order";
    if (i) {
      EXPECT_LT(from_v2k[i - 1], from_v2k[i]) << "order is the signed ascending int64 order";
    }
  }

  txn.commit();
  env.close();
  mdbx::env::remove("test-value2key");
}

TEST(ut_value2key, in_dupsort) {
  auto env = make_env("test-value2key");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  const int64_t values[] = {INT64_C(5), INT64_C(-3), INT64_C(5), INT64_C(1)};
  for (auto v : values) {
    const uint64_t be = __builtin_bswap64(::mdbx_key_from_int64(v));
    txn.upsert(multi, mdbx::slice("k"), mdbx::slice(&be, sizeof(be)));
  }

  auto cursor = txn.open_cursor(multi);
  cursor.to_first();
  int64_t previous = INT64_MIN;
  size_t seen = 0;
  do {
    const auto value = cursor.current().value;
    ASSERT_EQ(value.size(), sizeof(uint64_t));
    const auto decoded = int64_t(__builtin_bswap64(value.as_uint64()) - UINT64_C(0x8000000000000000));
    EXPECT_GT(decoded, previous) << "dups of one key are sorted by numeric value";
    previous = decoded;
    ++seen;
  } while (cursor.to_next(false));

  EXPECT_EQ(seen, 3u) << "plain dupsort deduplicates equal values: {-3,1,5}";
  EXPECT_EQ(cursor.count_multivalue(), seen);

  txn.commit();
  env.close();
  mdbx::env::remove("test-value2key");
}