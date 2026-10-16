#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/87
// mdbx_jsonInteger_from_key() decoded the key of 0 as INT64_MAX: the encoder
// maps 0 to the bias (0x8000000000000000), but the decoder split positive from
// negative on `key > bias`, so `key == bias` fell into the negative branch,
// the exponent wrapped to all-ones and the "too large" branch fired.

TEST(issue_gh0087, zero_roundtrips) {
  const int64_t vals[] = {0, 1, -1, 42, -42, INT64_MAX, INT64_MIN};
  for (const int64_t expected : vals) {
    const uint64_t key = mdbx_key_from_jsonInteger(expected);
    MDBX_val kv;
    kv.iov_base = const_cast<uint64_t *>(&key);
    kv.iov_len = sizeof(key);
    const int64_t actual = mdbx_jsonInteger_from_key(kv);
    EXPECT_EQ(expected, actual) << "jsonInteger " << expected << " round-trip";
  }
}
