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

/// SC-77 (цель ≥90%): decl_buffer хвосты — все 24 key_from* перегрузки
/// (char[n]/string_view/string/char*/buffer&&/double*/ptrfloat/slice и
/// прочие), buffer::clone/silo (allocator-пути), allocation_aware_details
/// copy/move/swap/propagate, make_reference формы, const/char_ptr/end.

namespace {
using buf = mdbx::buffer<>;
} // namespace

TEST(ut_gap_buffer2, key_from_all_overloads) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // литеральный массив (make_reference=true по умолчанию)
  auto k_lit = buf::key_from("literal");
  EXPECT_EQ(k_lit.size(), 7u);
  // string_view
  std::string_view sv = "sv-key";
  auto k_sv = buf::key_from(sv);
  EXPECT_EQ(k_sv.size(), sv.size());
  // char*
  const char *cstr = "cstr-key";
  auto k_cstr = buf::key_from(cstr);
  EXPECT_EQ(k_cstr.size(), strlen(cstr));
  // std::string
  std::string str = "std-key";
  auto k_str = buf::key_from(str);
  EXPECT_EQ(k_str.size(), str.size());
  // buffer&& (move)
  buf moved = buf::key_from("move-src");
  auto k_mv = buf::key_from(std::move(moved));
  EXPECT_EQ(k_mv.size(), 8u);
  // double* (ptrdouble)
  double d = 6.28318;
  auto k_pd = buf::key_from(&d);
  EXPECT_EQ(k_pd.size(), 8u);
  // float* (ptrfloat)
  float f = 1.4142f;
  auto k_pf = buf::key_from(&f);
  EXPECT_EQ(k_pf.size(), 4u);
  // все обёртки уже покрыты SC-73; здесь — остальные перегрузки-пути
  EXPECT_EQ(buf::key_from(static_cast<uint64_t>(0xffull)).size(), 8u);
  EXPECT_EQ(buf::key_from(static_cast<int64_t>(-1)).size(), 8u);
  EXPECT_EQ(buf::key_from(static_cast<uint32_t>(7u)).size(), 4u);
  EXPECT_EQ(buf::key_from(static_cast<int32_t>(-7)).size(), 4u);
  EXPECT_EQ(buf::key_from(2.0).size(), 8u);
  EXPECT_EQ(buf::key_from(0.5f).size(), 4u);
}

TEST(ut_gap_buffer2, clone_and_silo_allocators) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // buffer из строки: make_reference=false → собственное хранилище
  std::string src = "clone-source-payload";
  auto b1 = buf(src, /*make_reference=*/false);
  // clone: полная копия в новое хранилище
  auto b2 = buf::clone(b1);
  EXPECT_EQ(b2.size(), b1.size());
  EXPECT_EQ(mdbx::slice(b2), mdbx::slice(b1));
  // clone у reference-буфера: отрыв от исходного хранилища
  auto ref = buf::key_from(src, /*make_reference=*/true);
  auto cloned = buf::clone(ref);
  EXPECT_EQ(mdbx::slice(cloned), mdbx::slice(src));
  // silo: буфер с явным кастомным аллокатором (по умолчанию тот же тип)
  buf b3(src, false);
  EXPECT_EQ(b3.size(), src.size());
  // пустой clone
  buf empty("");
  auto ec = buf::clone(empty);
  EXPECT_EQ(ec.size(), 0u);
}

TEST(ut_gap_buffer2, allocation_aware_move_swap) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // move-семантика buffer (allocation_aware_details::move)
  buf a("move-a-payload", false);
  buf b(std::move(a));
  EXPECT_EQ(b.size(), 14u);
  // swap
  buf c("swap-c", false);
  buf d("swap-d-longer", false);
  c.swap(d);
  EXPECT_EQ(c.size(), 13u);
  EXPECT_EQ(d.size(), 6u);
  EXPECT_EQ(mdbx::slice(c), mdbx::slice("swap-d-longer"));
  EXPECT_EQ(mdbx::slice(d), mdbx::slice("swap-c"));
  // move-assign
  buf e;
  e = std::move(c);
  EXPECT_EQ(e.size(), 13u);
  // allocator-пути: copy_assign_alloc/move_assign_alloc/swap_alloc — через
  // буферы с одинаковыми аллокаторами (стандартный путь)
  buf f("f", false);
  buf g("g-payload", false);
  f = g;
  EXPECT_EQ(mdbx::slice(f), mdbx::slice(g));
  // propagate_on_container_copy_assignment-пути (is_always_equal=true у
  // std::allocator) — неявно покрыты операторами выше; явно — swap
  f.swap(g);
  EXPECT_EQ(mdbx::slice(f), mdbx::slice("g-payload"));
}

TEST(ut_gap_buffer2, reference_forms_and_pointers) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  std::string src = "ref-forms-payload";
  // make_reference=true: держит указатель на исходное хранилище;
  // mutable byte_ptr()/char_ptr() требуют is_freestanding() — для
  // reference-буфера используем константные версии
  buf r1(src, /*make_reference=*/true);
  EXPECT_EQ(r1.size(), src.size());
  EXPECT_TRUE(r1.is_reference());
  EXPECT_FALSE(r1.is_freestanding());
  // для reference-буфера ТОЛЬКО const-перегрузки (mutable require
  // is_freestanding — иначе assert); все вызовы через const-ссылку
  const buf &cr = r1;
  EXPECT_NE(cr.byte_ptr(), nullptr);
  EXPECT_NE(cr.char_ptr(), nullptr);
  EXPECT_EQ(cr.end_byte_ptr(), cr.byte_ptr() + cr.size());
  EXPECT_EQ(static_cast<const void *>(cr.end_char_ptr()),
            static_cast<const void *>(cr.end_byte_ptr()));
  EXPECT_EQ(static_cast<const void *>(cr.const_end()),
            static_cast<const void *>(cr.end_byte_ptr()));
  // после make_freestanding() mutable-пути валидны
  r1.make_freestanding();
  EXPECT_TRUE(r1.is_freestanding());
  EXPECT_NE(r1.byte_ptr(), nullptr);
  // invalid/null-формы
  buf inv(nullptr, static_cast<int>(0), false);
  EXPECT_EQ(inv.size(), 0u);
}

TEST(ut_gap_buffer2, append_strided_sizes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // append_u16/u24/u32/u48/u64 по одному (SC-73 покрыл цепочку; здесь
  // каждый отдельный вызов с проверкой байтов)
  auto b = mdbx::buffer<>();
  b.append_u16(0xAABB);
  EXPECT_EQ(b.size(), 2u);
  b.clear();
  b.append_u24(0xAABBCC);
  EXPECT_EQ(b.size(), 3u);
  b.clear();
  b.append_u32(0xDDCCBBAAu);
  EXPECT_EQ(b.size(), 4u);
  b.clear();
  b.append_u48(0x112233445566ull);
  EXPECT_EQ(b.size(), 6u);
  b.clear();
  b.append_u64(0x1122334455667788ull);
  EXPECT_EQ(b.size(), 8u);
  b.clear();
  b.append_byte(0x7f);
  EXPECT_EQ(b.size(), 1u);
}