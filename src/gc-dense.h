/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

/** \file gc-dense.h — решение задачи плотной упаковки возвращаемых страниц в слоты GC
 * (извлечено из gc-put.c, 2026-11, кампания верификации — для white-box тестирования
 * tests/ut/api/details_gc_dense_solve.c). Листовой заголовок: требует только essentials.h;
 * гистограмма передаётся плоскими параметрами (массив счётчиков + end), без связи с gc.h. */

#include "essentials.h"

/* Решение задачи плотной упаковки: покрыть объём volume страниц, используя не более slots
 * доступных слотов/идентификаторов, вырезая куски (GC-записи со спаном len, т.е. размещённые
 * на len смежных страницах) из прогонов смежных страниц гистограммы gc_dense_hist().
 *
 * Ключевое наблюдение, сводящее задачу к закрытой форме:
 *  1. Вместимость куска строго линейна по спану: per_chunk(len) = first_page + other_pages*(len-1),
 *     т.е. other_pages*len - (other_pages - first_page). Следовательно, покрытие любого набора кусков
 *     есть other_pages*sum(len_i) - (other_pages-first_page)*k, где k - число кусков.
 *  2. Прогон длиной L можно нарезать на любые куски с суммой спанов <= L (остатки нарезки остаются
 *     смежными и пригодны для дальнейших кусков). Поэтому любые k кусков покрывают не более
 *     other_pages*P(k) - (other_pages-first_page)*k, где P(k) - сумма длин k наибольших прогонов,
 *     и эта величина достигается взятием прогонов целиком.
 *
 * Отсюда точный критерий разрешимости: задача разрешима тогда и только тогда, когда существует
 * k <= slots с other_pages*P(k) - (other_pages-first_page)*k >= объёма. Проверяется сканом префиксных
 * сумм отсортированных (подсчётом) длин прогонов - без какого-либо перебора.
 *
 * Решение строится в двух режимах:
 *  - Равномерный (предпочтительный): минимальный спан m, при котором объём покрывается n(m) =
 *    ceil(объём / per_chunk(m)) кусками спана m, размещаемыми в прогонах (floor(len/m) кусков из
 *    прогона длиной len) с учётом числа слотов. Такие записи однородны и максимально коротки, что
 *    упрощает их последующую переработку. Может не существовать при дефиците слотов.
 *  - Минимум слотов (точный): жадно от самых длинных прогонов целыми кусками, последний кусок
 *    подрезается до минимально необходимого спана. Существует всегда, когда задача разрешима.
 *
 * Параметры:
 *  - hist_counts[31] / hist_end — счётчики прогонов (hist_counts[i] = прогоны длиной i+1) и граница;
 *  - solution_counts[31] / *solution_end — результирующие счётчики кусков по спанам.
 *
 * Возвращает 0 и *found = true/false (решение найдено/задача неразрешима), либо -2 при провале
 * самопроверки решения (недостижимо при корректных данных, сигнализирует о внутренней ошибке). */
MDBX_MAYBE_UNUSED static int gc_dense_pack(size_t slots, size_t volume, const pgno_t *const hist_counts,
                                           const unsigned hist_end, size_t first_page, size_t other_pages,
                                           pgno_t *const solution_counts, unsigned *const solution_end, bool *found) {
  ASSERT(slots > 0 && volume > 0);
  ASSERT(hist_counts != nullptr && solution_counts != nullptr && solution_end != nullptr && found != nullptr);
  if (!slots || !volume)
    return -1;

  for (unsigned i = 0; i < 31; ++i)
    solution_counts[i] = 0;

  /* Режим А (равномерный): сканируем спаны от коротких к длинам прогонов. */
  unsigned span = 0;
  for (unsigned m = 1; m <= 31; ++m) {
    const size_t per_chunk = first_page + other_pages * (m - 1);
    const size_t n = (volume + per_chunk - 1) / per_chunk;
    if (n > slots)
      continue;
    size_t pieces = 0;
    for (unsigned len = m; len <= hist_end; ++len)
      pieces += (size_t)hist_counts[len - 1] * (len / m);
    if (n <= pieces) {
      span = m;
      solution_counts[m - 1] = (pgno_t)n;
      break;
    }
  }

  if (!span) {
    /* Режим Б (минимум слотов): точный, существует всегда при разрешимости задачи. */
    size_t remaining = volume, used_slots = 0;
    unsigned max_span = 0;
    for (unsigned len = hist_end; len >= 1 && remaining > 0; --len) {
      const size_t per_chunk = first_page + other_pages * (len - 1);
      for (pgno_t c = hist_counts[len - 1]; c > 0 && remaining > 0; --c) {
        unsigned cut = len;
        if (per_chunk > remaining) {
          /* подрезаем последний кусок до минимально необходимого спана */
          cut = (unsigned)(1 + (remaining > first_page ? (remaining - first_page + other_pages - 1) / other_pages : 0));
          ASSERT(cut >= 1 && cut <= len);
        }
        solution_counts[cut - 1] += 1;
        if (cut > max_span)
          max_span = cut;
        remaining -= (per_chunk < remaining) ? per_chunk : remaining;
        used_slots += 1;
      }
    }
    if (remaining > 0 || used_slots > slots) {
      /* решение НЕ найдено */
      *found = false;
      return 0;
    }
    span = max_span;
  }

  {
    unsigned end = 31;
    while (end > 0 && solution_counts[end - 1] == 0)
      --end;
    *solution_end = end;
    (void)span;
  }

  /* проверяем решение */
  size_t items = 0, covered = 0;
  for (size_t i = 0, chunk = first_page; i < *solution_end; ++i) {
    items += solution_counts[i];
    covered += solution_counts[i] * chunk;
    chunk += other_pages;
  }
  if (unlikely(covered < volume || items > slots))
    return -2;
  *found = true;
  return 0;
}
