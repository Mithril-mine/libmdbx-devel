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

#include <iostream>

static int doit() {
  mdbx::path db_filename = "test-maindb-ordinal";
  mdbx::env_managed::remove(db_filename);
  mdbx::env_managed env(db_filename, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters());

  using buffer = mdbx::buffer<mdbx::default_allocator, mdbx::default_capacity_policy>;
  auto txn = env.start_write();
  auto map = txn.create_map(nullptr, mdbx::key_mode::ordinal, mdbx::value_mode::single);
#if 0 /* workaround */
  txn.commit();
  env.close();
  env = mdbx::env_managed(db_filename, mdbx::env_managed::create_parameters(),
                        mdbx::env::operate_parameters());
  txn = env.start_write();
#endif

  txn.insert(map, buffer::key_from_u64(UINT64_C(8) << 8 * 0), buffer("a"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(7) << 8 * 1), buffer("b"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(6) << 8 * 2), buffer("c"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(5) << 8 * 3), buffer("d"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(4) << 8 * 4), buffer("e"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(3) << 8 * 5), buffer("f"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(2) << 8 * 6), buffer("g"));
  txn.insert(map, buffer::key_from_u64(UINT64_C(1) << 8 * 7), buffer("h"));
  txn.commit();

  txn = env.start_read();
  auto cursor = txn.open_cursor(map);
#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L
  if (cursor.to_first().value.string_view() == "a" && cursor.to_next().value.string_view() == "b" &&
      cursor.to_next().value.string_view() == "c" && cursor.to_next().value.string_view() == "d" &&
      cursor.to_next().value.string_view() == "e" && cursor.to_next().value.string_view() == "f" &&
      cursor.to_next().value.string_view() == "g" && cursor.to_next().value.string_view() == "h" &&
      !cursor.to_next(false).done && cursor.eof()) {
    std::cout << "OK\n";
    return EXIT_SUCCESS;
  }
  std::cerr << "Fail\n";
  return EXIT_FAILURE;
#else
  std::cerr << "Skipped since no std::string_view\n";
  return EXIT_SUCCESS;
#endif /* __cpp_lib_string_view >= 201606L */
}

TEST(ut_maindb_ordinal, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  try {
    ASSERT_EQ(EXIT_SUCCESS, doit());
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    FAIL();
  }
}
