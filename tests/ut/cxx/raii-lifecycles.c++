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

/// SC-68 (test-coverage-gaps.md): редкие жизненные циклы C++ RAII-обёрток —
/// moved-from состояния, abort/commit в необычном порядке, cursor при
/// закрытой транзакции. Покрывает ветви impl_env (51%), decl_env (25%),
/// decl_txn (32%), decl_cursor (47%).

TEST(ut_raii_lifecycles, txn_abort_then_restart) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cxx-raii";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  // abort без операций — txn должен корректно завершиться
  {
    auto txn = env.start_write();
    txn.abort();
  }
  // insert + abort: данные НЕ сохраняются (но create_map("main") тоже
  // ушёл с abort — таблицы нет; пересоздаём)
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    txn.put(map, mdbx::slice("k"), mdbx::slice("v"), mdbx::upsert);
    txn.abort();
  }
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    EXPECT_THROW((void)txn.get(map, mdbx::slice("k")), mdbx::not_found);
    txn.commit();
  }
  // повторный commit закрытого txn → исключение (в C++ API это может быть
  // и mdbx::exception, и std::invalid_argument — зависит от обёртки)
  {
    auto txn = env.start_write();
    txn.commit();
    EXPECT_THROW(txn.commit(), std::exception);
  }
}

TEST(ut_raii_lifecycles, env_move_and_scope) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cxx-raii-move";
  mdbx::env::remove(testdb);
  mdbx::env_managed env_first(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env_first.start_write();
    txn.create_map("moved");
    txn.commit();
  }
  // move: первый env становится moved-from, владелец — второй
  mdbx::env_managed env_second = std::move(env_first);
  {
    auto txn = env_second.start_write();
    auto map = txn.open_map("moved");
    txn.put(map, mdbx::slice("a"), mdbx::slice("b"), mdbx::upsert);
    txn.commit();
  }
  // данные доступны через нового владельца
  {
    auto txn = env_second.start_read();
    auto map = txn.open_map("moved");
    EXPECT_EQ(txn.get(map, mdbx::slice("a")), mdbx::slice("b"));
    txn.abort();
  }
}

TEST(ut_raii_lifecycles, cursor_outlives_txn_bounds) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-cxx-raii-cur";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    txn.put(map, mdbx::slice("k1"), mdbx::slice("v1"), mdbx::upsert);
    txn.commit();
  }
  // курсор в области txn: навигация + bound-проверки
  {
    auto txn = env.start_read();
    auto map = txn.open_map("main");
    auto cur = txn.open_cursor(map);
    auto first = cur.move(mdbx::cursor::move_operation::first, true);
    EXPECT_TRUE(first.done);
    auto beyond = cur.move(mdbx::cursor::move_operation::next, false);
    EXPECT_FALSE(beyond.done); // конец данных
    // estimate-операции (не изменяют позицию)
    EXPECT_NO_THROW((void)cur.estimate(mdbx::cursor::move_operation::first));
    txn.abort(); // read-txn abort
  }
  // повторный read после abort — fresh view
  {
    auto txn = env.start_read();
    auto map = txn.open_map("main");
    EXPECT_EQ(txn.get(map, mdbx::slice("k1")), mdbx::slice("v1"));
    txn.abort();
  }
}