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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

/// SC-63: decl_transcoders — полное покрытие типов C++ (buffer/slice round-trip).
TEST(ut_transcoders_matrix, pod_and_views_roundtrip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const uint64_t u64 = 0x1122334455667788ull;
  const uint32_t u32 = 0xdeadbeefu;
  const uint16_t u16 = 0xbeefu;
  const uint8_t u8 = 0x7f;
  const float f = 3.14159f;
  const double d = 2.718281828459045;

  // slice → as_pod / as_N
  auto s = mdbx::slice(&u64, sizeof(u64));
  EXPECT_EQ(s.as_pod<uint64_t>(), u64);
  EXPECT_EQ(s.as_uint64(), u64);
  EXPECT_THROW(s.as_uint32(), mdbx::bad_value_size);

  auto s32 = mdbx::slice(&u32, sizeof(u32));
  EXPECT_EQ(s32.as_uint32(), u32);
  auto s16 = mdbx::slice(&u16, sizeof(u16));
  EXPECT_EQ(s16.as_uint16(), u16);
  auto s8 = mdbx::slice(&u8, sizeof(u8));
  EXPECT_EQ(s8.as_uint8(), u8);

  // float/double
  auto sf = mdbx::slice(&f, sizeof(f));
  EXPECT_EQ(sf.as_pod<float>(), f);
  auto sd = mdbx::slice(&d, sizeof(d));
  EXPECT_EQ(sd.as_pod<double>(), d);

  // buffer round-trip из slice (копирование модальности)
  mdbx::buffer buf(s);
  EXPECT_EQ(buf.size(), s.size());
  EXPECT_EQ(buf.as_pod<uint64_t>(), u64);
  auto s_back = mdbx::slice(buf);
  EXPECT_EQ(s_back.as_pod<uint64_t>(), u64);
}

TEST(ut_transcoders_matrix, string_and_vector_modalities) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string str = "hello transcoder";
  mdbx::buffer from_str(mdbx::slice(str), /*make_reference=*/false);
  EXPECT_EQ(from_str.size(), str.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(from_str.byte_ptr()),
                          from_str.length()), str);

  const std::vector<uint8_t> vec = {1, 2, 3, 4, 5};
  mdbx::buffer from_vec(mdbx::slice(vec.data(), vec.size()),
                        /*make_reference=*/false);
  EXPECT_EQ(from_vec.size(), vec.size());
  EXPECT_EQ(*from_vec.byte_ptr(), vec[0]);
}

/// SC-64: decl_core — исключения ↔ коды C API.
TEST(ut_transcoders_matrix, error_mapping) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-transcoders-err";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    // несуществующий ключ → mdbx::not_found (бросает: get() = slice)
    EXPECT_THROW((void)txn.get(map, mdbx::slice("absent-key")), mdbx::not_found);
    txn.put(map, mdbx::slice("k1"), mdbx::slice("v1"), mdbx::upsert);
    EXPECT_EQ(txn.get(map, mdbx::slice("k1")), mdbx::slice("v1"));
    txn.commit();
  }
  // сверхлимитный ключ → mdbx::exception
  {
    auto txn = env.start_write();
    auto map = txn.open_map("main");
    const size_t key_limit = mdbx::env::limits::key_max(
        env, mdbx::key_mode::usual);
    std::string big_key(key_limit + 1, 'x');
    EXPECT_THROW(txn.put(map, mdbx::slice(big_key), mdbx::slice("v"), mdbx::upsert),
                 mdbx::exception);
    txn.abort();
  }
}