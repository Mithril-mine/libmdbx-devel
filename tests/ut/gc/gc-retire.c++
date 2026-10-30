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

#include <random>
#include <system_error>
#include <filesystem>

/// SC-65 (test-coverage-gaps.md): gc-get/gc-put — цепочки ретирирования.
/// Повторяющиеся массовые удаления/вставки формируют GC-записи (retire →
/// reclaim/pipelining); страницы возвращаются в переиспользование — размер
/// файла стабилизируется, mdbx_chk-эквивалент (последовательные txn-инварианты
/// под MDBX_DEBUG/AUDIT) не нарушается.
TEST(ut_gap_gc, retire_reclaim_cycles) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-gc-retire";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  std::mt19937 rng(42);
  const size_t records = 8000;
  const size_t val_size = 128; // крупные записи → страницы заполняются быстро

  // Цикл 1..3: наполнение → массовое удаление → commit.
  // Каждая итерация создаёт GC-записи (ретирированные страницы), которые
  // обязаны переиспользоваться в следующих итерациях.
  for (int cycle = 0; cycle < 3; ++cycle) {
    {
      auto txn = env.start_write();
      auto map = txn.create_map("main");
      for (size_t i = 0; i < records; ++i) {
        const uint32_t k = static_cast<uint32_t>((i * 2654435769u + cycle) & 0xffffffu);
        std::string key(16, 'k');
        memcpy(&key[0], &k, sizeof(k));
        std::string val(val_size, 'v');
        snprintf(&val[1], val_size - 1, "%zu", i);
        txn.put(map, mdbx::slice(key), mdbx::slice(val), mdbx::upsert);
      }
      txn.commit();
    }
    {
      auto txn = env.start_write();
      auto map = txn.open_map("main");
      // массовое удаление: каждая нечётная запись (bunch-режим)
      for (size_t i = 0; i < records; i += 2) {
        const uint32_t k = static_cast<uint32_t>((i * 2654435769u + cycle) & 0xffffffu);
        std::string key(16, 'k');
        memcpy(&key[0], &k, sizeof(k));
        (void)txn.erase(map, mdbx::slice(key));
      }
      txn.commit();
    }
  }

  // Финальная очистка + чистое наполнение: ожидаем ровно records записей
  {
    auto txn = env.start_write();
    auto map = txn.open_map("main");
    txn.clear_map(map);
    for (size_t i = 0; i < records; ++i) {
      const uint32_t k = static_cast<uint32_t>((i * 2654435769u) & 0xffffffu);
      std::string key(16, 'k');
      memcpy(&key[0], &k, sizeof(k));
      std::string val(val_size, 'v');
      snprintf(&val[1], val_size - 1, "%zu", i);
      txn.put(map, mdbx::slice(key), mdbx::slice(val), mdbx::upsert);
    }
    txn.commit();
  }
  {
    auto txn = env.start_read();
    auto map = txn.open_map("main");
    auto cur = txn.open_cursor(map);
    size_t n = 0;
    auto r = cur.move(mdbx::cursor::move_operation::first, false);
    while (r.done) {
      ++n;
      r = cur.move(mdbx::cursor::move_operation::next, false);
    }
    EXPECT_EQ(n, records); // все записи живы после GC-циклов
  }

  // Переиспользование: после 3 циклов наполнение+удаление размер файла
  // должен быть разумным (страницы переиспользованы, а не накапливаться).
  // Лимит вычисляется от СИСТЕМНОЙ страницы, не от константы: на macOS
  // страница 16K (против 4K на Linux) — точная арифметика файла зависит
  // от платформы (см. замечание в test-coverage-gaps.md).
  {
    // фактическая страница БД (macOS: 16K, Linux: 4K) — лимит обязан
    // зависеть от неё, не от константы
    const uintmax_t page = static_cast<uintmax_t>(env.get_pagesize());
    // Размер файла определяется не только данными (~1.2МБ payload'а), но и
    // динамикой роста: gc_alloc_ex резервирует growth-инкремент (по умолчанию
    // до 4096 страниц), а shrink возвращается к нему же. Поэтому честный
    // платформо-независимый ceiling — страницы-под-данные (×8 на накладные
    // B+tree/GC) с округлением вверх до страниц файла.
    const uintmax_t payload = records * (16 + val_size);
    const uintmax_t data_pages = (payload * 8 + page - 1) / page;
    const uintmax_t growth_pages = 4096; // дефолтный growth-инкремент libmdbx
    const uintmax_t limit = ((data_pages + growth_pages) * page);
    uintmax_t fs = 0;
    std::error_code ec;
    for (const auto &p : std::filesystem::directory_iterator(".")) {
      const auto name = p.path().filename().string();
      if (name.rfind("test-gap-gc-retire", 0) == 0) {
        const auto sz = std::filesystem::file_size(p.path(), ec);
        if (!ec)
          fs += sz;
      }
    }
    EXPECT_GT(fs, 0u);
    EXPECT_LT(fs, limit) << "pagesize=" << page << " payload=" << payload;
  }
}