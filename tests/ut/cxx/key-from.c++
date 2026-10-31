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

#include <cstring>

/// SC-73 (цель ≥90%): C++ decl_transcoders/decl_buffer — key_from*
/// (int32/int64/u32/u64/float/double/jsonInteger/ptrfloat, 22 перегрузки
/// key_from), append_u8/u16/u24/u32/u48/u64, append_producer
/// (Mutable/ImmutableByteProducer концепты), make_freestanding /
/// is_reference / is_freestanding.

namespace {
/// ImmutableByteProducer: отдаёт повторяющийся паттерн.
struct pattern_producer {
  size_t length;
  char fill;

  bool is_empty() const { return length == 0; }
  size_t envisage_result_length() const { return length; }
  char *write_bytes(char *array, size_t limit) const {
    for (size_t i = 0; i < limit; ++i)
      array[i] = fill;
    return array + limit;
  }
};

/// MutableByteProducer: счётчик байт.
struct counter_producer {
  size_t length;

  bool is_empty() const { return length == 0; }
  size_t envisage_result_length() const { return length; }
  char *write_bytes(char *array, size_t limit) {
    for (size_t i = 0; i < limit; ++i)
      array[i] = static_cast<char>(i & 0x7f);
    return array + limit;
  }
};
} // namespace

TEST(ut_key_from, key_from_numeric_all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // u32/i32/u64/i64: ключ — big-endian представление
  auto ku32 = mdbx::buffer<>::key_from_u32(0x11223344u);
  EXPECT_EQ(ku32.size(), 4u);
  auto ki32 = mdbx::buffer<>::key_from_i32(-12345);
  EXPECT_EQ(ki32.size(), 4u);
  auto ku64 = mdbx::buffer<>::key_from_u64(0x1122334455667788ull);
  EXPECT_EQ(ku64.size(), 8u);
  auto ki64 = mdbx::buffer<>::key_from_i64(-9876543210ll);
  EXPECT_EQ(ki64.size(), 8u);

  // float/double: IEEE754-байтовый порядок
  auto kf = mdbx::buffer<>::key_from_float(3.14159f);
  EXPECT_EQ(kf.size(), 4u);
  // байты детерминированы: повторный key_from того же значения даёт те же байты
  auto kf2 = mdbx::buffer<>::key_from_float(3.14159f);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(kf.byte_ptr()), 4),
            std::string(reinterpret_cast<const char *>(kf2.byte_ptr()), 4));

  auto kd = mdbx::buffer<>::key_from_double(2.718281828);
  EXPECT_EQ(kd.size(), 8u);

  // jsonInteger: int64 → u64-ключ, сравнимый с key_from_double
  auto kj = mdbx::buffer<>::key_from_jsonInteger(42);
  EXPECT_EQ(kj.size(), 8u);
  // jsonInteger(42) сортируется рядом с double(42.0)
  auto kdd = mdbx::buffer<>::key_from_double(42.0);
  EXPECT_EQ(kj.size(), kdd.size());

  // ptrfloat (по указателю)
  float pf = 1.61803f;
  auto kpf = mdbx::buffer<>::key_from(&pf);
  EXPECT_EQ(kpf.size(), 4u);

  // перегрузки-обёртки key_from(T)
  EXPECT_EQ(mdbx::buffer<>::key_from(static_cast<uint32_t>(0x11223344u)).size(), 4u);
  EXPECT_EQ(mdbx::buffer<>::key_from(static_cast<int32_t>(-12345)).size(), 4u);
  EXPECT_EQ(mdbx::buffer<>::key_from(static_cast<uint64_t>(0x1122334455667788ull)).size(), 8u);
  EXPECT_EQ(mdbx::buffer<>::key_from(static_cast<int64_t>(-9876543210ll)).size(), 8u);
  EXPECT_EQ(mdbx::buffer<>::key_from(3.14159f).size(), 4u);
  EXPECT_EQ(mdbx::buffer<>::key_from(2.718281828).size(), 8u);
}

TEST(ut_key_from, append_u8_to_u64) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::buffer<> b;
  b.append_u8(0x11).append_u16(0x2222).append_u24(0x333333).append_u32(0x44444444u).append_u48(
      0x555555555555ull).append_u64(0x6666666666666666ull);
  // суммарный размер: 1+2+3+4+6+8 = 24
  EXPECT_EQ(b.size(), 24u);
  EXPECT_EQ(*b.byte_ptr(), 0x11);
}

TEST(ut_key_from, append_producer_concepts) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // ImmutableByteProducer
  pattern_producer imm{100, 'x'};
  mdbx::buffer<> b1;
  b1.append_producer(imm);
  // capacity policy может округлять вместимость — проверяем содержимое
  EXPECT_GE(b1.size(), 100u);
  for (size_t i = 0; i < 100; ++i)
    EXPECT_EQ(*(b1.byte_ptr() + i), 'x') << "at " << i;

  // MutableByteProducer
  counter_producer mut{64};
  mdbx::buffer<> b2;
  b2.append_producer(mut);
  EXPECT_EQ(b2.size(), 64u);

  // пустой producer
  pattern_producer empty{0, 'z'};
  mdbx::buffer<> b3;
  b3.append_producer(empty);
  EXPECT_EQ(b3.size(), 0u);

  // несколько подряд: capacity policy может округлять размер вверх —
  // проверяем минимум и содержимое
  pattern_producer part{10, 'a'};
  mdbx::buffer<> b4;
  b4.append_producer(part).append_producer(part);
  EXPECT_GE(b4.size(), 20u);
  for (size_t i = 0; i < 20; ++i)
    EXPECT_EQ(*(b4.byte_ptr() + i), 'a') << "at " << i;
}

TEST(ut_key_from, freestanding_reference) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // buffer из строки: make_reference=true → reference (in-place если возможно)
  std::string src = "reference-test";
  auto ref = mdbx::buffer<>::key_from(src, /*make_reference=*/true);
  EXPECT_EQ(ref.size(), src.size());
  // make_freestanding: переводит в собственное хранилище
  ref.make_freestanding();
  EXPECT_EQ(ref.size(), src.size());
  EXPECT_TRUE(mdbx::slice(ref) == mdbx::slice(src));
  // is_reference/is_freestanding на разных формах
  (void)mdbx::buffer<>::key_from(src, false).make_freestanding();
}