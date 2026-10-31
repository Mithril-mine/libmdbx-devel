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

/// SC-72 (test-coverage-gaps.md): хвосты C++ decl_buffer/decl_transcoders —
/// MDBX_commit_latency::operator= (вложенные анонимные структуры, C-only
/// компилируются только в C++-сборке), buffer::append_base58/base64 и
/// append_decoded_base64/hex — 471+153 непокрытых блока.

TEST(ut_commit_latency, commit_latency_assign) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cla";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  mdbx::txn_managed::finalization_latency a{}, b{};
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    txn.put(map, mdbx::slice("k"), mdbx::slice("v"), mdbx::upsert);
    // commit с latency: заполняет все поля (в т.ч. gc-статистику)
    txn.commit(&a);
  }
  // operator= вложенных анонимных структур (top-level + вложенная gc-стата)
  b.preparation = a.preparation;
  b.gc_wallclock = a.gc_wallclock;
  b.write = a.write;
  b.ending = a.ending;
  b.whole = a.whole;
  b.gc_prof.wloops = a.gc_prof.wloops;
  b.gc_prof.work_counter = a.gc_prof.work_counter;
  EXPECT_EQ(b.whole, a.whole);
  EXPECT_EQ(b.gc_prof.wloops, a.gc_prof.wloops);
  // целиком оператор= структуры
  mdbx::txn_managed::finalization_latency c = a;
  EXPECT_EQ(c.whole, a.whole);
  // латентности > 0 при реальном commit (стены времени идут вперёд)
  EXPECT_GT(a.whole, 0u);
}

TEST(ut_commit_latency, base58_base64_roundtrip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // данные с разными паттернами байт (нули, границы, повторения)
  const std::string payloads[] = {
      "",                       // пустой
      "\x00\x01\x02",           // ведущие нули
      "The quick brown fox",    // текст
      "\xff\xfe\xfd\xfc\xfb",   // старшие байты
      std::string(300, '\x5a'), // длинный одинаковый (wrap_width путь)
  };
  for (const auto &payload : payloads) {
    mdbx::slice src(payload);
    // base58
    auto b58 = mdbx::buffer().append_base58(src);
    auto dec58 = mdbx::buffer().append_decoded_base58(b58);
    EXPECT_EQ(mdbx::slice(dec58), src) << "base58 roundtrip len=" << payload.size();
    // base64 (+wrap_width ветвь переносов строк)
    auto b64 = mdbx::buffer().append_base64(src, 40);
    auto dec64 = mdbx::buffer().append_decoded_base64(b64, true);
    EXPECT_EQ(mdbx::slice(dec64), src) << "base64 roundtrip len=" << payload.size();
    // base64 без wrap
    auto b64n = mdbx::buffer().append_base64(src, 0);
    auto dec64n = mdbx::buffer().append_decoded_base64(b64n, false);
    EXPECT_EQ(mdbx::slice(dec64n), src);
  }
  // base64 с пробелами/переносами внутри (ignore_spaces=true)
  auto spaced = mdbx::buffer().append_base64(mdbx::slice("foobar"), 4);
  auto dec = mdbx::buffer().append_decoded_base64(spaced, true);
  EXPECT_EQ(mdbx::slice(dec), mdbx::slice("foobar"));
}

TEST(ut_commit_latency, hex_append_paths) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string payload = "\x01\x23\x45\x67\x89\xab\xcd\xef";
  mdbx::slice src(payload);
  // lowercase / uppercase / wrap_width
  auto low = mdbx::buffer().append_hex(src, false, 0);
  EXPECT_EQ(low.size(), src.size() * 2);
  auto up = mdbx::buffer().append_hex(src, true, 0);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(up.byte_ptr()), up.length()), "0123456789ABCDEF");
  auto wrapped = mdbx::buffer().append_hex(src, false, 4);
  EXPECT_GE(wrapped.size(), src.size() * 2); // переносы добавляют байты
  // hex-декодирование
  auto dec = mdbx::buffer().append_decoded_hex(up);
  EXPECT_EQ(mdbx::slice(dec), src);
}