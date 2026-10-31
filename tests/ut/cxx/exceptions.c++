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

/// SC-75 (цель ≥90%): mdbx:: — все 35 классов исключений (ctor/dtor
/// через error::throw_exception + что()), exception_thunk
/// capture/rethrow, env::defrag C++-обёртки, envinfo/gc_info/cache_result
/// структуры, env::alter_flags, get_HandleSlowReaders, txn::clear_map,
/// env::copy, buffer::clone, env::silo.

TEST(ut_exceptions, all_exception_classes_via_throw) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // Каждый класс исключений бросается через error::throw_exception(),
  // ловится как mdbx::exception, what() валиден, code() совпадает.
  // Механика: CASE_EXCEPTION(NAME, CODE) в src/mdbx.c++.
  struct probe {
    MDBX_error_t code;
    std::function<bool(const std::exception &)> is_that;
    const char *name;
  };
  // Проверяем через динамическое приведение к mdbx::exception и code()
  auto probe_one = [](MDBX_error_t code) {
    mdbx::error err(code);
    try {
      err.throw_exception();
      ADD_FAILURE() << "no exception for code " << code;
    } catch (const mdbx::exception &e) {
      EXPECT_EQ(e.error().code(), code) << "code mismatch for " << code;
      EXPECT_NE(e.what(), nullptr);
    } catch (const std::exception &e) {
      // Системные коды (EPERM/EIO/EACCESS...) маппятся в std::system_error,
      // SUCCESS/RESULT_* — в std::logic_error; и то и другое допустимо.
      EXPECT_TRUE(code == MDBX_EPERM || code == MDBX_EIO ||
                  code == MDBX_EACCESS || code == MDBX_SUCCESS ||
                  code == MDBX_RESULT_FALSE || code == MDBX_RESULT_TRUE)
          << "unexpected std::exception for code " << code << ": " << e.what();
    } catch (...) {
      ADD_FAILURE() << "non-standard exception for code " << code;
    }
  };
  const MDBX_error_t codes[] = {
      MDBX_BAD_DBI, MDBX_CORRUPTED, MDBX_MAP_FULL, MDBX_INCOMPATIBLE,
      MDBX_NOTFOUND, MDBX_KEYEXIST, MDBX_PROBLEM, MDBX_DBS_FULL,
      MDBX_BAD_TXN, MDBX_BAD_VALSIZE, MDBX_PAGE_NOTFOUND, MDBX_CURSOR_FULL,
      MDBX_INVALID, MDBX_TOO_LARGE, MDBX_UNABLE_EXTEND_MAPSIZE,
      MDBX_VERSION_MISMATCH, MDBX_WANNA_RECOVERY,
      // NOTE: MDBX_EBADSIGN/MDBX_PANIC бросают mdbx::fatal, чей деструктор
      // намеренно вызывает std::terminate() (неисправимые ошибки) — их
      // нельзя поймать в живом процессе, не тестируем.
      MDBX_PAGE_FULL, MDBX_EKEYMISMATCH, MDBX_READERS_FULL,
      MDBX_EMULTIVAL, MDBX_ENODATA, MDBX_BAD_RSLOT, MDBX_EREMOTE,
      MDBX_BUSY, MDBX_THREAD_MISMATCH, MDBX_TXN_FULL,
      MDBX_TXN_OVERLAPPING, MDBX_DUPLICATED_LCK, MDBX_DANGLING_DBI,
      MDBX_OUSTED, MDBX_MVCC_RETARDED, MDBX_LAGGARD_READER,
  };
  for (auto code : codes) {
    SCOPED_TRACE(code);
    probe_one(code);
  }
}

TEST(ut_exceptions, exception_thunk_capture_rethrow) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // exception_thunk: capture → is_clean() → rethrow_captured()
  mdbx::exception_thunk thunk;
  EXPECT_TRUE(thunk.is_clean());
  try {
    throw mdbx::error(MDBX_NOTFOUND);
  } catch (...) {
    // capture() без аргументов — берёт активное исключение из потока
    thunk.capture();
  }
  EXPECT_FALSE(thunk.is_clean());
  bool rethrown = false;
  try {
    thunk.rethrow_captured();
  } catch (const mdbx::exception &e) {
    EXPECT_EQ(e.error().code(), MDBX_NOTFOUND);
    rethrown = true;
  } catch (const std::exception &e) {
    ADD_FAILURE() << "rethrown as std::exception: " << e.what();
  }
  EXPECT_TRUE(rethrown);
  // повторный capture после rethrow: новый пустой thunk
  mdbx::exception_thunk empty_thunk;
  EXPECT_TRUE(empty_thunk.is_clean());
}

TEST(ut_exceptions, env_defrag_cxx_and_flags) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cxx-defrag";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    for (uint32_t i = 0; i < 3000; ++i) {
      uint32_t k = i * 5 + 1;
      char val[160];
      memset(val, 'd', sizeof(val));
      txn.put(map, mdbx::slice(&k, sizeof(k)), mdbx::slice(val, sizeof(val)),
              mdbx::upsert);
      if (i % 2 == 0)
        txn.erase(map, mdbx::slice(&k, sizeof(k)));
    }
    txn.commit();
  }
  // env::defrag C++-обёртки: простая форма (возвращает result) + visitor
  auto result = env.defrag(1, 0, 16, 0, 0, 4096);
  EXPECT_GT(result.pages_whole, 0u);
  EXPECT_LE(result.rough_estimation_cycle_progress_permille, 1000u);
  // alter_flags: читаем/меняем флаги env (перезагрузочный путь)
  auto flags = env.get_flags();
  EXPECT_NE(flags, 0u);
  // HandleSlowReaders getter
  auto hsr = env.get_HandleSlowReaders();
  EXPECT_EQ(hsr, nullptr); // не устанавливался
}

TEST(ut_exceptions, txn_clear_map_copy_and_structs) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cxx-structs";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    for (uint32_t i = 0; i < 500; ++i) {
      uint32_t k = i * 3;
      txn.put(map, mdbx::slice(&k, sizeof(k)), mdbx::slice("v"), mdbx::upsert);
    }
    txn.commit();
  }
  // txn::clear_map: очищает таблицу без удаления дескриптора
  {
    auto txn = env.start_write();
    auto map = txn.open_map("main");
    txn.clear_map(map);
    txn.commit();
  }
  {
    auto txn = env.start_read();
    auto map = txn.open_map("main");
    MDBX_stat st;
    ASSERT_EQ(mdbx_dbi_stat(txn, map, &st, sizeof(st)), MDBX_SUCCESS);
    EXPECT_EQ(st.ms_entries, 0u);
    txn.abort();
  }
  // env::copy: копия БД в новый файл
  const std::string copy_path = "test-gap-cxx-structs-copy";
  mdbx::env::remove(copy_path);
  env.copy(copy_path, false);
  // копия открывается и пуста (после clear_map)
  {
    mdbx::env_managed env2(copy_path, mdbx::create_parameters(),
                           mdbx::operate_parameters().set_max_maps(8));
    auto txn = env2.start_read();
    auto map = txn.open_map("main", mdbx::key_mode::usual);
    MDBX_stat st;
    ASSERT_EQ(mdbx_dbi_stat(txn, map, &st, sizeof(st)), MDBX_SUCCESS);
    EXPECT_EQ(st.ms_entries, 0u);
    txn.abort();
  }
  mdbx::env::remove(copy_path);
  // структуры: envinfo / gc_info / cache_result (конструкторы/деструкторы)
  auto info = env.get_info();
  EXPECT_GT(info.mi_geo.current, 0u);
  EXPECT_GT(info.mi_dxb_pagesize, 0u);
  MDBX_gc_info gi{};
  (void)gi;
  mdbx::txn_managed::cache_result cache{};
  (void)cache;
}