/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025

#include "internals.h"

#include "gc-store-retired.h"

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
MDBX_INTERNAL int gc_dense_pack(size_t slots, size_t volume, const pgno_t *const hist_counts, const unsigned hist_end,
                                size_t first_page, size_t other_pages, pgno_t *const solution_counts,
                                unsigned *const solution_end, bool *found) {
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

int gc_put_init(MDBX_txn *txn, gcu_t *ctx) {
  memset(ctx, 0, offsetof(gcu_t, sequel));
  /* Размер куска помещающийся на одну отдельную "overflow" страницу, но с небольшим запасом сводобного места. */
  ctx->goodchunk = txn->env->maxgc_large1page - (txn->env->maxgc_large1page >> 4);
  rkl_init(&ctx->sequel);
#if MDBX_ENABLE_BIGFOOT
  ctx->bigfoot = txn->txnid;
#endif /* MDBX_ENABLE_BIGFOOT */
  return cursor_init(&ctx->cursor, txn, FREE_DBI);
}

void gc_put_destroy(gcu_t *ctx) { rkl_destroy(&ctx->sequel); }

static size_t gc_chunk_pages(const MDBX_txn *txn, const size_t chunk) {
  return largechunk_npages(txn->env, gc_chunk_bytes(chunk));
}

static int gc_peekid(const MDBX_val *key, txnid_t *id) {
  if (likely(key->iov_len == sizeof(txnid_t))) {
    *id = unaligned_peek_u64(4, key->iov_base);
    return MDBX_SUCCESS;
  }
  ERROR("%s/%d: %s", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid GC key-length");
  return MDBX_CORRUPTED;
}

#if MDBX_DEBUG_GCU
#pragma push_macro("LOG_ENABLED")
#undef LOG_ENABLED
#define LOG_ENABLED(LVL)                                                                                               \
  unlikely(MDBX_DEBUG_GCU > 2 || (ctx && ctx->loop > 1 && (MDBX_DEBUG_GCU > 1 || LVL < MDBX_LOG_EXTRA)) ||             \
           LVL <= globals.loglevel)
#endif /* MDBX_DEBUG_GCU */

MDBX_MAYBE_UNUSED static void dbg_id(gcu_t *ctx, txnid_t id) {
#if MDBX_DEBUG_GCU
  if (ctx->dbg.prev) {
    if (ctx->dbg.prev != id - 1) {
      if (ctx->dbg.n)
        DEBUG_EXTRA_PRINT("-%" PRIaTXN, ctx->dbg.prev);
      if (id)
        DEBUG_EXTRA_PRINT(" %" PRIaTXN, id);
      ctx->dbg.n = 0;
    } else
      ctx->dbg.n += 1;
  } else {
    DEBUG_EXTRA_PRINT(" %" PRIaTXN, id);
    ctx->dbg.n = 0;
  }
  ctx->dbg.prev = id;
#else
  (void)ctx;
  (void)id;
#endif /* MDBX_DEBUG_GCU */
}

MDBX_MAYBE_UNUSED static void dbg_dump_ids(gcu_t *ctx) {
#if MDBX_DEBUG_GCU
  if (LOG_ENABLED(MDBX_LOG_EXTRA)) {
    DEBUG_EXTRA("%s", "GC:");
    if (ctx->cursor.tree->items) {
      cursor_couple_t couple;
      MDBX_val key;
      int err = cursor_init(&couple.outer, ctx->cursor.txn, FREE_DBI);
      if (err != MDBX_SUCCESS)
        ERROR("%s(), %d", "cursor_init", err);
      else
        err = outer_first(&couple.outer, &key, nullptr);

      txnid_t id;
      while (err == MDBX_SUCCESS) {
        err = gc_peekid(&key, &id);
        if (unlikely(err == MDBX_SUCCESS)) {
          dbg_id(ctx, id);
          if (id > couple.outer.txn->env->gc.detent)
            break;
          err = outer_next(&couple.outer, &key, nullptr, MDBX_NEXT);
        }
      }
      dbg_id(ctx, 0);
      DEBUG_EXTRA_PRINT("%s\n", (id > couple.outer.txn->env->gc.detent) ? "..." : "");
    } else
      DEBUG_EXTRA_PRINT("%s\n", " empty");

    DEBUG_EXTRA("%s", "ready4reuse:");
    if (rkl_empty(&ctx->cursor.txn->wr.gc.ready4reuse))
      DEBUG_EXTRA_PRINT("%s\n", " empty");
    else {
      rkl_iter_t i = rkl_iterator(&ctx->cursor.txn->wr.gc.ready4reuse, false);
      txnid_t id = rkl_turn(&i, false);
      while (id) {
        dbg_id(ctx, id);
        id = rkl_turn(&i, false);
      }
      dbg_id(ctx, 0);
      DEBUG_EXTRA_PRINT("%s\n", "");
    }

    DEBUG_EXTRA("%s", "comeback:");
    if (rkl_empty(&ctx->cursor.txn->wr.gc.comeback))
      DEBUG_EXTRA_PRINT("%s\n", " empty");
    else {
      rkl_iter_t i = rkl_iterator(&ctx->cursor.txn->wr.gc.comeback, false);
      txnid_t id = rkl_turn(&i, false);
      while (id) {
        dbg_id(ctx, id);
        id = rkl_turn(&i, false);
      }
      dbg_id(ctx, 0);
      DEBUG_EXTRA_PRINT("%s\n", "");
    }
  }
#else
  (void)ctx;
#endif /* MDBX_DEBUG_GCU */
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline size_t gc_reclaimed_maxspan_chunk(MDBX_txn *txn,
                                                                                             gcu_t *ctx) {
  (void)ctx;
  /* Функция вычисляет размер куска списка возвращаемых/неиспользованных в GC страниц, который можно разместить
   * в последовательности смежных страниц, которая возможно есть в самом этом списке.
   *
   * С одной стороны, такое размещение позволяет обойтись меньшим кол-вом слотов при возврате страниц,
   * уменьшить кол-во итераций при резервировании, а также не дробить саму последовательность.
   *
   * Однако, при последующей переработке такая тактика допустима только при не-отложенной очистке GC. Иначе
   * последовательность смежных страниц останется в GC до завершения переработавшей эту запись транзакции. А затем, в
   * ходе обновления GC при фиксации транзакции, будет раздроблена или снова возвращена в GC. Таким образом, без
   * не-отложенной очистки, такая тактика способствует миграции последовательностей страниц внутрь структуры самой GC
   * делая его более громоздкой.
   *
   * Поэтому пока используем только при нехватке слотов. */
  const size_t maxspan = pnl_maxspan(txn->wr.repnl);
  tASSERT0(txn, maxspan > 0);
  size_t start_lp = txn->env->maxgc_large1page /* начало набольшой страницы, с заголовком */;
  size_t tail_lp =
      ((maxspan - 1) << txn->env->ps2ln) / sizeof(txnid_t) /* продолжение большой страницы, без заголовка */;
  size_t pages4span = maxspan /* кол-во страниц для размещения*/;
  size_t chunk = start_lp + tail_lp - pages4span;
  TRACE("maxspan %zu, chunk %zu: %zu (start_lp) + %zu (tail_lp) - %zu (pages4span))", maxspan, chunk, start_lp, tail_lp,
        pages4span);
  return chunk;
}

MDBX_INTERNAL int gc_clean_stored_retired(MDBX_txn *txn, gcu_t *ctx) {
  int err = MDBX_SUCCESS;
  if (ctx->retired_stored) {
    do {
      MDBX_val key, val;
#if MDBX_ENABLE_BIGFOOT
      key.iov_base = &ctx->bigfoot;
#else
      key.iov_base = &txn->txnid;
#endif /* MDBX_ENABLE_BIGFOOT */
      key.iov_len = sizeof(txnid_t);
      const csr_t csr = cursor_seek(&ctx->cursor, &key, &val, MDBX_SET);
      if (csr.err == MDBX_SUCCESS && csr.exact) {
        ctx->retired_stored = 0;
        err = cursor_del(&ctx->cursor, 0);
        TRACE("== clear-4linear @%" PRIaTXN ", stockpile %zu, err %d", *(txnid_t *)key.iov_base, gc_stockpile(txn),
              err);
      } else
        err = (csr.err == MDBX_NOTFOUND) ? MDBX_SUCCESS : csr.err;
    }
#if MDBX_ENABLE_BIGFOOT
    while (!err && --ctx->bigfoot >= txn->txnid);
#else
    while (0);
#endif /* MDBX_ENABLE_BIGFOOT */
  }
  return err;
}

static int gc_touch(gcu_t *ctx) {
  tASSERT0(ctx->cursor.txn, is_pointed(&ctx->cursor) || ctx->cursor.txn->dbs[FREE_DBI].leaf_pages == 0);
  MDBX_val key, val;
  key.iov_base = val.iov_base = nullptr;
  key.iov_len = sizeof(txnid_t);
  val.iov_len = MDBX_PNL_SIZEOF(ctx->cursor.txn->wr.retired_pages);
  ctx->cursor.flags |= z_gcu_preparation;
  int err = cursor_touch(&ctx->cursor, &key, &val);
  ctx->cursor.flags -= z_gcu_preparation;
  return err;
}

static inline int gc_reclaim_slot(MDBX_txn *txn, gcu_t *ctx) {
  (void)txn;
  return gc_alloc_ex(&ctx->cursor, 0, ALLOC_RESERVE | ALLOC_UNIMPORTANT).err;
}

static inline int gc_reserve4retired(MDBX_txn *txn, gcu_t *ctx, size_t sequence_length) {
  (void)txn;
  return gc_alloc_ex(&ctx->cursor, sequence_length, ALLOC_RESERVE | ALLOC_UNIMPORTANT).err;
}

static inline int gc_reserve4stockpile(MDBX_txn *txn, gcu_t *ctx) {
  (void)txn;
  return gc_alloc_ex(&ctx->cursor, 1, ALLOC_RESERVE | ALLOC_UNIMPORTANT).err;
}

static int gc_prepare_stockpile(MDBX_txn *txn, gcu_t *ctx, const size_t for_retired) {
  for (;;) {
    tASSERT0(txn, is_pointed(&ctx->cursor) || txn->dbs[FREE_DBI].leaf_pages == 0);

    const size_t for_cow = txn->dbs[FREE_DBI].height;
    const size_t for_rebalance = for_cow + 1 + (txn->dbs[FREE_DBI].height + 1ul >= txn->dbs[FREE_DBI].branch_pages);
    const size_t for_tree_before_touch = for_cow + for_rebalance;
    const size_t for_tree_after_touch = for_rebalance;
    const size_t for_all_before_touch = for_retired + for_tree_before_touch;
    const size_t for_all_after_touch = for_retired + for_tree_after_touch;

    if (likely(for_retired < 2 && gc_stockpile(txn) > for_all_before_touch))
      return MDBX_SUCCESS;

    TRACE(">> retired-stored %zu, retired-left %zi, stockpile %zu, now-need %zu (4list %zu, "
          "4cow %zu, 4tree %zu)",
          ctx->retired_stored, pnl_size(txn->wr.retired_pages) - ctx->retired_stored, gc_stockpile(txn),
          for_all_before_touch, for_retired, for_cow, for_tree_before_touch);

    int err = gc_touch(ctx);
    TRACE("== after-touch, stockpile %zu, err %d", gc_stockpile(txn), err);

    if (!MDBX_ENABLE_BIGFOOT && unlikely(for_retired > 1) && pnl_size(txn->wr.retired_pages) != ctx->retired_stored &&
        err == MDBX_SUCCESS) {
      if (unlikely(ctx->retired_stored)) {
        err = gc_clean_stored_retired(txn, ctx);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        if (!ctx->retired_stored)
          continue;
      }
      err = gc_reserve4retired(txn, ctx, for_retired);
      TRACE("== after-4linear, stockpile %zu, err %d", gc_stockpile(txn), err);
      cASSERT1(&ctx->cursor, gc_stockpile(txn) >= for_retired || err != MDBX_SUCCESS);
    }

    while (gc_stockpile(txn) < for_all_after_touch && err == MDBX_SUCCESS)
      err = gc_reserve4stockpile(txn, ctx);

    TRACE("<< stockpile %zu, err %d, gc: height %u, branch %zu, leaf %zu, large "
          "%zu, entries %zu",
          gc_stockpile(txn), err, txn->dbs[FREE_DBI].height, (size_t)txn->dbs[FREE_DBI].branch_pages,
          (size_t)txn->dbs[FREE_DBI].leaf_pages, (size_t)txn->dbs[FREE_DBI].large_pages,
          (size_t)txn->dbs[FREE_DBI].items);
    return (err != MDBX_NOTFOUND) ? err : MDBX_SUCCESS;
  }
}

static int gc_prepare_stockpile4update(MDBX_txn *txn, gcu_t *ctx) { return gc_prepare_stockpile(txn, ctx, 0); }

MDBX_INTERNAL int gc_prepare_stockpile4retired(MDBX_txn *txn, gcu_t *ctx) {
  const size_t retired_whole = pnl_size(txn->wr.retired_pages);
  const intptr_t retired_left = retired_whole - ctx->retired_stored;
  size_t for_retired = 0;
  if (retired_left > 0) {
    if (unlikely(!ctx->retired_stored)) {
      /* Make sure last page of GC is touched and on retired-list */
      int err = outer_last(&ctx->cursor, nullptr, nullptr);
      if (unlikely(err != MDBX_SUCCESS) && err != MDBX_NOTFOUND)
        return err;
      for_retired += 1;
    }
    if (MDBX_ENABLE_BIGFOOT) {
      const size_t per_branch_page = txn->env->maxgc_per_branch;
      for_retired += (retired_left + ctx->goodchunk - 1) / ctx->goodchunk;
      for (size_t entries = for_retired; entries > 1; for_retired += entries)
        entries = (entries + per_branch_page - 1) / per_branch_page;
    } else
      /* The whole list is stored as one value, which takes a page for each gc_chunk_bytes() of it. */
      for_retired += gc_chunk_pages(txn, retired_whole);
  }

  return gc_prepare_stockpile(txn, ctx, for_retired);
}

int gc_merge_loose(MDBX_txn *txn, gcu_t *ctx) {
  (void)ctx;
  tASSERT0(txn, txn->wr.loose_count > 0);
  /* Return loose page numbers to wr.repnl, though usually none are left at this point.
   * The pages themselves remain in dirtylist. */
  if (unlikely(!(txn->dbi_state[FREE_DBI] & DBI_DIRTY)) && txn->wr.loose_count < 3 + (unsigned)txn->dbs->height * 2) {
    /* Put loose page numbers in wr.retired_pages, since unreasonable to return ones to wr.repnl. */
    TRACE("merge %zu loose-pages into %s-pages", txn->wr.loose_count, "retired");
    int err = pnl_need(&txn->wr.retired_pages, txn->wr.loose_count);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    for (page_t *lp = txn->wr.loose_pages; lp; lp = page_next(lp)) {
      pnl_append_prereserved(txn->wr.retired_pages, lp->pgno);
      MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(lp), sizeof(page_t *));
      VALGRIND_MAKE_MEM_DEFINED(&page_next(lp), sizeof(page_t *));
    }
  } else {
    /* Room for loose pages + temp PNL with same */
    TRACE("merge %zu loose-pages into %s-pages", txn->wr.loose_count, "reclaimed");
    int err = pnl_need(&txn->wr.repnl, 2 * txn->wr.loose_count + 2);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    pnl_t loose = txn->wr.repnl + pnl_alloclen(txn->wr.repnl) - txn->wr.loose_count - 1;
    size_t count = 0;
    for (page_t *lp = txn->wr.loose_pages; lp; lp = page_next(lp)) {
      tASSERT0(txn, lp->flags == P_LOOSE);
      loose[++count] = lp->pgno;
      MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(lp), sizeof(page_t *));
      VALGRIND_MAKE_MEM_DEFINED(&page_next(lp), sizeof(page_t *));
    }
    tASSERT0(txn, count == txn->wr.loose_count);
    pnl_setsize(loose, count);
    pnl_sort(loose, txn->geo.first_unallocated);
    pnl_merge(txn->wr.repnl, loose);
  }

  /* filter-out list of dirty-pages from loose-pages */
  dpl_t *const dl = txn->wr.dirtylist;
  if (dl) {
    tASSERT0(txn, (txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);
    tASSERT0(txn, dl->sorted <= dl->length);
    size_t w = 0, sorted_out = 0;
    for (size_t r = w; ++r <= dl->length;) {
      page_t *dp = dl->items[r].ptr;
      tASSERT0(txn, dp->flags == P_LOOSE || is_modifiable(txn, dp));
      tASSERT0(txn, dpl_endpgno(dl, r) <= txn->geo.first_unallocated);
      if ((dp->flags & P_LOOSE) == 0) {
        if (++w != r)
          dl->items[w] = dl->items[r];
      } else {
        tASSERT0(txn, dp->flags == P_LOOSE);
        sorted_out += dl->sorted >= r;
        if (!MDBX_AVOID_MSYNC || !(txn->flags & MDBX_WRITEMAP))
          page_shadow_release(txn->env, dp, 1);
      }
    }
    TRACE("filtered-out loose-pages from %zu -> %zu dirty-pages", dl->length, w);
    tASSERT0(txn, txn->wr.loose_count == dl->length - w);
    dl->sorted -= sorted_out;
    tASSERT0(txn, dl->sorted <= w);
    dpl_setlen(dl, w);
    dl->pages_including_loose -= txn->wr.loose_count;
    txn->wr.dirtyroom += txn->wr.loose_count;
    tASSERT0(txn, txn->wr.dirtyroom + txn->wr.dirtylist->length ==
                      (txn->parent ? txn->parent->wr.dirtyroom : txn->env->options.dp_limit));
  } else {
    tASSERT0(txn, (txn->flags & MDBX_WRITEMAP) != 0 && !MDBX_AVOID_MSYNC);
  }
  txn->wr.loose_pages = nullptr;
  txn->wr.loose_count = 0;
  txn->wr.loose_refund_wl = 0;
  return MDBX_SUCCESS;
}

static int gc_remove_rkl(MDBX_txn *txn, gcu_t *ctx, rkl_t *rkl) {
  while (!rkl_empty(rkl)) {
    txnid_t id = rkl_edge(rkl, gc_is_lifo(txn));
    if (ctx->gc_first == id)
      ctx->gc_first = 0;
    tASSERT0(txn, id <= txn->env->lck->cached_oldest_txnid.weak);
#ifndef _MSC_VER
    MDBX_val key = {.iov_base = &id, .iov_len = sizeof(id)};
#else
    /* avoid MSVC crash and/or ICE */
    MDBX_val key;
    key.iov_base = &id;
    key.iov_len = sizeof(id);
#endif
    int err = cursor_seek(&ctx->cursor, &key, nullptr, MDBX_SET).err;
    tASSERT0(txn, id == rkl_edge(rkl, gc_is_lifo(txn)));
    if (err == MDBX_NOTFOUND) {
      err = rkl_push(&txn->wr.gc.ready4reuse, rkl_pop(rkl, gc_is_lifo(txn)));
      WARNING("unexpected %s for gc-id %" PRIaTXN ", ignore and continue, push-err %d", "MDBX_NOTFOUND", id, err);
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
      continue;
    }
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    err = gc_prepare_stockpile4update(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (unlikely(id != rkl_edge(rkl, gc_is_lifo(txn)))) {
      TRACE("id %" PRIaTXN " not at edge, continue", id);
      continue;
    }
    err = cursor_del(&ctx->cursor, 0);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    ENSURE_OBJ(txn->env, id == rkl_pop(rkl, gc_is_lifo(txn)));
    tASSERT0(txn, id <= txn->env->lck->cached_oldest_txnid.weak);
    err = rkl_push(&txn->wr.gc.ready4reuse, id);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    TRACE("id %" PRIaTXN " cleared and moved to ready4reuse", id);
  }
  return MDBX_SUCCESS;
}

static inline int gc_clear_reclaimed(MDBX_txn *txn, gcu_t *ctx) {
  return gc_remove_rkl(txn, ctx, &txn->wr.gc.reclaimed);
}

static inline int gc_clear_returned(MDBX_txn *txn, gcu_t *ctx) {
  ctx->return_reserved_lo = 0;
  ctx->return_reserved_hi = 0;
  return gc_remove_rkl(txn, ctx, &txn->wr.gc.comeback);
}

static int gc_push_sequel(MDBX_txn *txn, gcu_t *ctx, txnid_t id) {
  tASSERT0(txn, id > 0 && id <= txn->env->gc.detent);
  tASSERT1(txn, !rkl_contain(&txn->wr.gc.comeback, id) && !rkl_contain(&txn->wr.gc.ready4reuse, id));
  TRACE("id %" PRIaTXN ", return-left %zi", id, ctx->return_left);
  int err = rkl_push(&ctx->sequel, id);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (err == MDBX_RESULT_TRUE) {
      ERROR("%s/%d: %s", "MDBX_PROBLEM", MDBX_PROBLEM, "unexpected duplicate(s) during rkl-push");
      err = MDBX_PROBLEM;
    }
    return err;
  }
  ctx->return_left -= ctx->goodchunk;
  return (ctx->return_left <= 0) ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

/* Строит гистограмму длин последовательностей соседствующих/примыкающих страниц */
static void gc_dense_hist(MDBX_txn *txn, gcu_t *ctx) {
  memset(&ctx->dense_histogram, 0, sizeof(ctx->dense_histogram));
  size_t seqlen = 0, seqmax = 1;
  for (size_t i = 2; i <= pnl_size(txn->wr.repnl); ++i) {
    seqlen += 1;
    if (seqlen == ARRAY_LENGTH(ctx->dense_histogram.array) ||
        !MDBX_PNL_CONTIGUOUS(txn->wr.repnl[i - 1], txn->wr.repnl[i], 1)) {
      ctx->dense_histogram.array[seqlen - 1] += 1;
      seqmax = (seqmax >= seqlen) ? seqmax : seqlen;
      seqlen = 0;
    }
  }
  ctx->dense_histogram.array[seqlen] += 1;
  ctx->dense_histogram.end = (unsigned)((seqmax > seqlen) ? seqmax : seqlen + 1);
}

static int gc_dense_solve(MDBX_txn *txn, gcu_t *ctx, gc_dense_histogram_t *const solution) {
  const size_t slots = rkl_len(&txn->wr.gc.ready4reuse);
  ASSERT(slots > 0 && ctx->return_left > 0 && pnl_size(txn->wr.repnl) > 0);
  if (unlikely(!slots || !ctx->return_left)) {
    ERROR("%s/%d: %s", "MDBX_PROBLEM", MDBX_PROBLEM, "recursive-solving preconditions violated");
    return MDBX_PROBLEM;
  }

  const size_t first_page = /* на первой странице */ txn->env->maxgc_large1page +
                            /* сама страница также будет израсходована */ 1;
  const size_t other_pages = /* на второй и последующих страницах */ txn->env->ps / sizeof(pgno_t) +
                             /* каждая страница также будет израсходована */ 1;
  bool found = false;
  const int rc = gc_dense_pack(slots, (size_t)ctx->return_left, ctx->dense_histogram.array, ctx->dense_histogram.end,
                               first_page, other_pages, solution->array, &solution->end, &found);
  if (unlikely(rc < 0)) {
    ASSERT(!"recursive-solving failure");
    ERROR("%s/%d: %s", "MDBX_PROBLEM", MDBX_PROBLEM, "recursive-solving failure");
    return MDBX_PROBLEM;
  }
  return found ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

/* Ищем свободные/неиспользуемые id в GC, чтобы затем использовать эти идентификаторы для возврата неиспользованных
 * остатков номеров страниц ранее изъятых из GC.
 *
 * Нехватка идентификаторов достаточно редкая ситуация, так как возвращается страниц обычно не более чем было извлечено.
 * Однако, больше идентификаторов может потребоваться в следующих ситуациях:
 *
 *  - ранее с БД работала старая версия libmdbx без поддержки BigFoot и поэтому были переработано очень длинные записи,
 *    возврат остатков которых потребует нарезки на несколько кусков.
 *
 *  - ранее было зафиксировано несколько транзакций, которые помещали в GC retired-списки близкие к максимальному
 *    размеру помещающемуся на одну листовую страницу, после чего текущая транзакция переработала все эти записи,
 *    но по совокупности операций эти страницы оказались лишними и теперь при возврате потребуют больше слотов
 *    из-за обеспечения резерва свободного места при нарезке на множество кусков.
 *
 * Таким образом, потребность в поиске возникает редко и в большинстве случаев необходимо найти 1-2 свободных
 * слота/идентификатора. Если же требуется много слотов, то нет смысла экономить на поиске. */
static int gc_search_holes(MDBX_txn *txn, gcu_t *ctx) {
  tASSERT0(txn, ctx->return_left > 0 && txn->env->gc.detent);
  tASSERT0(txn, rkl_empty(&txn->wr.gc.reclaimed));
  if (!ctx->gc_first) {
    ctx->gc_first = txn->env->gc.detent;
    if (txn->dbs[FREE_DBI].items) {
      MDBX_val key;
      int err = outer_first(&ctx->cursor, &key, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      err = gc_peekid(&key, &ctx->gc_first);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
  }

  /* В LIFO режиме требуется поиск в направлении от новых к старым записям с углублением в потенциально
   * рыхлую/неоднородную структуру, с последующим её заполнением возвращаемыми страницами. */

  /* В FIFO режиме, поиск внутри GC может быть полезным при нелинейной переработке (которая пока не реализована),
   * когда будет переработан один из следующих MVCC-снимков без переработки предыдущего. Необходимая для этого
   * независимость (отсутствие пересечения) снимков по набору retired-страниц может сложиться при последовательности
   * пишущих транзакций изменяющих данные в структурно одних и тех же страницах b-tree.
   *
   * Однако, в текущем понимании, это крайне редкая ситуация, при которой также весьма вероятно наличие свободного
   * интервала в начале GC, а поэтому вероятность выигрыша от дополнительного поиска вперед стремится к нулю. Кроме
   * этого, из-за мизерной вероятности ситуаций в которых такой поиск будет работать, его крайне сложно тестировать
   * -- требуется разработка отдельного теста, который может быть достаточно хрупким, так как любая доработка
   * основного кода может требовать изменений/подстройки сценария теста.
   *
   * Поэтому пока, до появления явной необходимости и/или пользы, решено отказаться от поиска свободных слотов вглубь GC
   * в направлении от старых записей к новым, в том числе в режиме FIFO. */

  dbg_dump_ids(ctx);
  const intptr_t tail_space =
      (intptr_t)(((ctx->gc_first > UINT16_MAX) ? (size_t)UINT16_MAX : (size_t)ctx->gc_first - 1) * ctx->goodchunk);
  const txnid_t reasonable_deep =
      txn->env->maxgc_per_branch +
      2 * (txn->env->gc.detent - txnid_min(rkl_lowest(&txn->wr.gc.ready4reuse), rkl_lowest(&txn->wr.gc.comeback)));
  const txnid_t scan_threshold = (txn->env->gc.detent > reasonable_deep) ? txn->env->gc.detent - reasonable_deep : 0;

  txnid_t scan_hi = txn->env->gc.detent, scan_lo = INVALID_TXNID;
  if (!gc_is_lifo(txn) && ctx->gc_first < txn->env->gc.detent &&
      txn->env->gc.detent - ctx->gc_first < ctx->cursor.tree->items) {
    scan_hi = ctx->gc_first;
    scan_lo = 0;
  }

  rkl_iter_t iter_ready4reuse, iter_comeback;
  rkl_find(&txn->wr.gc.ready4reuse, scan_hi, &iter_ready4reuse);
  rkl_find(&txn->wr.gc.comeback, scan_hi, &iter_comeback);
  rkl_hole_t hole_ready4reuse = rkl_hole(&iter_ready4reuse, true);
  rkl_hole_t hole_comeback = rkl_hole(&iter_comeback, true);
  txnid_t begin, end;
  /* Ищем свободные id в GC в направлении от конца (новых записей) к началу (старым записям). */
  do {
    TRACE("hole-ready4reuse %" PRIaTXN "..%" PRIaTXN ", hole-comeback %" PRIaTXN "..%" PRIaTXN ", scan-range %" PRIaTXN
          "..%" PRIaTXN,
          hole_ready4reuse.begin, hole_ready4reuse.end, hole_comeback.begin, hole_comeback.end, scan_lo, scan_hi);
    MDBX_val key;
    int err;
    end = txnid_min(scan_hi, txnid_min(hole_ready4reuse.end, hole_comeback.end));
    if (hole_comeback.begin >= end) {
      hole_comeback = rkl_hole(&iter_comeback, true);
      TRACE("turn-comeback %" PRIaTXN "..%" PRIaTXN, hole_comeback.begin, hole_comeback.end);
    } else if (hole_ready4reuse.begin >= end) {
      hole_ready4reuse = rkl_hole(&iter_ready4reuse, true);
      TRACE("turn-ready4reuse %" PRIaTXN "..%" PRIaTXN, hole_ready4reuse.begin, hole_ready4reuse.end);
    } else if (scan_lo >= end) {
      TRACE("turn-scan from %" PRIaTXN "..%" PRIaTXN, scan_lo, scan_hi);
      scan_hi = scan_lo - 1;
      if (scan_lo - end > 4) {
        scan_lo = end - 1;
        key.iov_base = &scan_lo;
        key.iov_len = sizeof(scan_lo);
        const csr_t csr = cursor_seek(&ctx->cursor, &key, nullptr, MDBX_SET_RANGE);
        if (csr.err != MDBX_NOTFOUND) {
          if (unlikely(csr.err != MDBX_SUCCESS))
            return csr.err;
        }
        scan_hi = end - csr.exact;
      }
      goto scan;
    } else {
      begin = txnid_max(scan_lo, txnid_max(hole_ready4reuse.begin, hole_comeback.begin));
      tASSERT0(txn, begin <= scan_hi && begin > 0);
      while (--end >= begin) {
        err = gc_push_sequel(txn, ctx, end);
        tASSERT0(txn, (ctx->return_left > 0) == (err != MDBX_RESULT_TRUE));
        if (err != MDBX_SUCCESS) {
          return err;
        }
      }
      if (MIN_TXNID >= begin)
        break;
      if (begin == hole_comeback.begin) {
        hole_comeback = rkl_hole(&iter_comeback, true);
        TRACE("pull-comeback %" PRIaTXN "..%" PRIaTXN, hole_comeback.begin, hole_comeback.end);
      }
      if (begin == hole_ready4reuse.begin) {
        hole_ready4reuse = rkl_hole(&iter_ready4reuse, true);
        TRACE("pull-ready4reuse %" PRIaTXN "..%" PRIaTXN, hole_ready4reuse.begin, hole_ready4reuse.end);
      }
      if (begin == scan_lo) {
        TRACE("pull-scan from %" PRIaTXN "..%" PRIaTXN, scan_lo, scan_hi);
        do {
          scan_hi = scan_lo - 1;
        scan:
          if (scan_hi < scan_threshold && tail_space >= ctx->return_left) {
            /* Искать глубже нет смысла, ибо в начале GC есть достаточно свободных идентификаторов. */
            TRACE("stop-scan %s", "threshold");
            scan_lo = 0;
            scan_hi = ctx->gc_first;
            break;
          }
          err = outer_prev(&ctx->cursor, &key, nullptr, MDBX_PREV);
          if (err == MDBX_NOTFOUND) {
            /* больше нет записей ближе к началу GC, все значения id свободны */
            TRACE("stop-scan %s", "eof");
            scan_lo = 0;
            break;
          }
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          err = gc_peekid(&key, &scan_lo);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          TRACE("scan: peek %" PRIaTXN, scan_lo);
          scan_lo += 1;
        } while (scan_lo >= scan_hi);
        TRACE("scan-range %" PRIaTXN "..%" PRIaTXN, scan_lo, scan_hi);
      }
    }
  } while (end > MIN_TXNID);
  return MDBX_SUCCESS;
}

static inline int gc_reserve4return(MDBX_txn *txn, gcu_t *ctx, const size_t chunk_lo, const size_t chunk_hi) {
  txnid_t reservation_id = rkl_pop(&txn->wr.gc.ready4reuse, true);
  TRACE("%s: slots-ready4reuse-left %zu, reservation-id %" PRIaTXN, gc_dbg_prefix(ctx),
        rkl_len(&txn->wr.gc.ready4reuse), reservation_id);
  tASSERT0(txn, reservation_id >= MIN_TXNID && reservation_id < txn->txnid);
  tASSERT0(txn, reservation_id <= txn->env->lck->cached_oldest_txnid.weak);
  if (unlikely(reservation_id < MIN_TXNID ||
               reservation_id > atomic_load64(&txn->env->lck->cached_oldest_txnid, mo_Relaxed))) {
    ERROR("** internal error (reservation gc-id %" PRIaTXN ")", reservation_id);
    return MDBX_PROBLEM;
  }

  int err = rkl_push(&txn->wr.gc.comeback, reservation_id);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

#ifndef _MSC_VER
  MDBX_val key = {.iov_base = &reservation_id, .iov_len = sizeof(reservation_id)};
  MDBX_val data = {.iov_base = nullptr, .iov_len = gc_chunk_bytes(chunk_hi)};
#else
  /* avoid MSVC crash and/or ICE */
  MDBX_val key;
  key.iov_base = &reservation_id;
  key.iov_len = sizeof(reservation_id);
  MDBX_val data;
  data.iov_base = nullptr;
  data.iov_len = gc_chunk_bytes(chunk_hi);
#endif
  TRACE("%s: reserved +%zu...+%zu [%zu...%zu), err %d", gc_dbg_prefix(ctx), chunk_lo, chunk_hi,
        ctx->return_reserved_lo + 1, ctx->return_reserved_hi + chunk_hi + 1, err);
  err = gc_prepare_stockpile4update(txn, ctx);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  /* LY: ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE */
  /* coverity[OVERRUN] */
  err = cursor_put(&ctx->cursor, &key, &data, MDBX_RESERVE | MDBX_NOOVERWRITE);
  tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated - 1));
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  memset(data.iov_base, 0, data.iov_len);
  ctx->return_reserved_lo += chunk_lo;
  ctx->return_reserved_hi += chunk_hi;
  if (unlikely(!rkl_empty(&txn->wr.gc.reclaimed))) {
    NOTICE("%s: restart since %zu slot(s) reclaimed (reserved %zu...%zu of %zu)", gc_dbg_prefix(ctx),
           rkl_len(&txn->wr.gc.reclaimed), ctx->return_reserved_lo, ctx->return_reserved_hi, pnl_size(txn->wr.repnl));
    return MDBX_RESULT_TRUE;
  }

  return MDBX_SUCCESS;
}

static size_t dense_chunk_outlay(const MDBX_txn *txn, const size_t chunk) {
  size_t need_span = gc_chunk_pages(txn, chunk);
  return gc_repnl_has_span(txn, need_span) ? need_span : 0;
}

static size_t dense_adjust_chunk(const MDBX_txn *txn, const size_t chunk) {
  size_t adjusted = chunk;
  if (chunk > txn->env->maxgc_large1page) {
    size_t hi = chunk + 1, lo = chunk - gc_chunk_pages(txn, chunk) - 1;
    while (lo < hi) {
      adjusted = (hi + lo) / 2;
      size_t probe = chunk - dense_chunk_outlay(txn, adjusted);
      if (probe > adjusted)
        lo = adjusted + 1;
      else if (probe < adjusted)
        hi = adjusted - 1;
      else
        break;
    }
  }
  return adjusted;
}

static size_t dense_adjust_amount(const MDBX_txn *const txn, size_t amount) {
  const size_t gap = 2 + txn->dbs[FREE_DBI].height;
  const size_t snubber = txn->env->ps / sizeof(pgno_t) / 2;
  return ((amount + gap < txn->env->maxgc_large1page) ? txn->env->maxgc_large1page : amount + snubber);
}

static int gc_handle_dense(MDBX_txn *txn, gcu_t *ctx, size_t left_min, size_t left_max) {
  /* Крайне маловероятная ситуация, в текущем понимании не возможная при нормальной/ожидаемой работе всех
   * актуальных версий движка. Тем не менее, сюда мы можем попасть при использовании БД с содержимым GC
   * оставшимся после старых версий и/или при выключенном BigFoot. Тогда в GC могут быть записи огромного
   * размера, при возврате которых мы получаем так много кусков, что в GC не хватает свободных/неиспользуемых
   * идентификаторов от прошлых транзакций.
   *
   * Дальше три возможности:
   * 1. Искать в GC доступные для переработки записи короче maxgc_large1page. Это малоэффективный путь,
   *    так как если мы уже попали в текущую ситуацию, то маловероятно что в GC есть такие записи и что запаса
   *    места хватит. Поэтому оставляем этот путь в качестве предпоследнего варианта.
   * 2. Попытаться запихнуть остаток одним куском, который может быть многократно больше maxgc_large1page,
   *    т.е. потребуется несколько последовательных свободных страниц, из-за чего может произойти загрузка всей
   *    GC и т.д. Это плохой путь, который можно использовать только в качестве последнего шанса.
   * 3. Искать смежные страницы среди возвращаемых и сохранять куски помещающиеся в такие последовательности.
   *
   *  Поэтому комбинируем все три варианта 3+1+2:
   *  - Вычисляем среднюю целевую длину куска в large/overflow страницах, при нарезке на которые имеющихся
   *    слотов/идентификаторов хватит для размещения возвращаемых страниц.
   *  - В идеале ищем в wr.repnl последовательности смежных страниц длиной от ⌊целевой длины куска⌋
   *    до ⌈целевой длины куска⌉ и выполняем резервирование кусками помещающимися в эти последовательности.
   *    Теоретически, вероятность (а следовательно и количество) последовательностей экспоненциально уменьшается
   *    с увеличением длины. На практике, в основном, это будут пары и тройки страниц, но также и длинные
   *    последовательности, которые образуются в исходных больших транзакциях (порождающий большие
   *    retired-списки), особенно при выделении новых страниц. При этом использование длинных
   *    последовательностей чревато повторением проблем при переработке созданных сейчас записей.
   *  - Поэтому оптимальное решение выглядит как поиск набора последовательностей, мощность которого равна
   *    количеству доступных слотов/идентификаторов, а длины последовательностей минимальны, но достаточны для
   *    размещения всех возвращаемых страниц. */

  int err = MDBX_RESULT_FALSE;
  if (!rkl_empty(&txn->wr.gc.ready4reuse)) {
    gc_dense_hist(txn, ctx);
    gc_dense_histogram_t solution;
    if (ctx->loop == 1 || ctx->loop % 3 == 0)
      left_max = dense_adjust_amount(txn, left_max);
    ctx->return_left = left_max;
    err = gc_dense_solve(txn, ctx, &solution);
    if (err == MDBX_RESULT_FALSE /* решение НЕ найдено */ && left_max != left_min) {
      if (ctx->loop == 1 || ctx->loop % 3 == 0)
        left_min = dense_adjust_amount(txn, left_min);
      if (left_max != left_min) {
        ctx->return_left = left_min;
        err = gc_dense_solve(txn, ctx, &solution);
      }
    }
    if (err == MDBX_RESULT_TRUE /* решение найдено */) {
      for (size_t i = solution.end; i > 0; --i)
        for (pgno_t n = 0; n < solution.array[i - 1]; ++n) {
          size_t span = i;
          size_t chunk_hi = txn->env->maxgc_large1page + txn->env->ps / sizeof(pgno_t) * (span - 1);
          if (chunk_hi > left_max) {
            chunk_hi = left_max;
            span = gc_chunk_pages(txn, chunk_hi);
          }
          size_t chunk_lo = chunk_hi - txn->env->maxgc_large1page + ctx->goodchunk;
          TRACE("%s: dense-chunk (seq-len %zu, %d of %d) %zu...%zu, gc-per-ovpage %u", gc_dbg_prefix(ctx), i, n + 1,
                solution.array[i - 1], chunk_lo, chunk_hi, txn->env->maxgc_large1page);
          size_t amount = pnl_size(txn->wr.repnl);
          err = gc_reserve4return(txn, ctx, chunk_lo, chunk_hi);
          if (unlikely(err != MDBX_SUCCESS))
            return err;

          const size_t now = pnl_size(txn->wr.repnl);
          if (span < amount - now - txn->dbs[FREE_DBI].height || span > amount - now + txn->dbs[FREE_DBI].height)
            TRACE("dense-%s-reservation: miss %zu (expected) != %zi (got)", "solve", span, amount - now);
          amount = now;
          if (ctx->return_reserved_hi >= amount)
            return MDBX_SUCCESS;
          left_max = dense_adjust_amount(txn, amount) - ctx->return_reserved_lo;
        }
    }
  } else if (rkl_len(&txn->wr.gc.comeback)) {
    NOTICE("%s: restart since %zu slot(s) comeback non-dense (reserved %zu...%zu of %zu)", gc_dbg_prefix(ctx),
           rkl_len(&txn->wr.gc.comeback), ctx->return_reserved_lo, ctx->return_reserved_hi, pnl_size(txn->wr.repnl));
    return /* повтор цикла */ MDBX_RESULT_TRUE;
  }

  if (err == MDBX_RESULT_FALSE /* решение НЕ найдено, либо нет идентификаторов */) {
    if (ctx->return_left > (intptr_t)txn->env->maxgc_large1page) {
      err = gc_reclaim_slot(txn, ctx);
      if (err == MDBX_NOTFOUND)
        err = gc_reserve4retired(txn, ctx, gc_chunk_pages(txn, dense_adjust_chunk(txn, ctx->return_left)));
      if (err != MDBX_NOTFOUND && err != MDBX_SUCCESS)
        return err;
    }

    const size_t per_page = txn->env->ps / sizeof(pgno_t);
    size_t amount = pnl_size(txn->wr.repnl);
    do {
      if (rkl_empty(&txn->wr.gc.ready4reuse)) {
        NOTICE("%s: restart since no slot(s) available (reserved %zu...%zu of %zu)", gc_dbg_prefix(ctx),
               ctx->return_reserved_lo, ctx->return_reserved_hi, amount);
        return MDBX_RESULT_TRUE;
      }
      const size_t left = dense_adjust_amount(txn, amount) - ctx->return_reserved_hi;
      const size_t slots = rkl_len(&txn->wr.gc.ready4reuse);
      const size_t base = (left + slots - 1) / slots;
      const size_t adjusted = dense_adjust_chunk(txn, base);
      TRACE("dense-reservation: reserved %zu...%zu of %zu, left %zu slot(s) and %zu pnl, step: %zu base,"
            " %zu adjusted",
            ctx->return_reserved_lo, ctx->return_reserved_hi, amount, slots, left, base, adjusted);
      const size_t chunk_hi =
          (adjusted > txn->env->maxgc_large1page)
              ? txn->env->maxgc_large1page + ceil_powerof2(adjusted - txn->env->maxgc_large1page, per_page)
              : txn->env->maxgc_large1page;
      const size_t chunk_lo =
          (adjusted > txn->env->maxgc_large1page)
              ? txn->env->maxgc_large1page + floor_powerof2(adjusted - txn->env->maxgc_large1page, per_page)
              : adjusted;
      err = gc_reserve4return(txn, ctx, chunk_lo, chunk_hi);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      const size_t now = pnl_size(txn->wr.repnl);
      if (base - adjusted + txn->dbs[FREE_DBI].height < amount - now ||
          base - adjusted > amount - now + txn->dbs[FREE_DBI].height)
        TRACE("dense-%s-reservation: miss %zu (expected) != %zi (got)", "unsolve", base - adjusted, amount - now);
      amount = now;
    } while (ctx->return_reserved_hi < amount);
  }

  if (unlikely(err != MDBX_SUCCESS))
    ERROR("unable provide IDs and/or to fit returned PNL (%zd+%zd pages, %zd+%zd slots), err %d", ctx->retired_stored,
          pnl_size(txn->wr.repnl), rkl_len(&txn->wr.gc.comeback), rkl_len(&txn->wr.gc.ready4reuse), err);
  return err;
}

/* Выполняет один шаг резервирования записей для возврата в GC страниц (их номеров), оставшихся после
 * переработки GC и последующего использования в транзакции. */
static int gc_rerere(MDBX_txn *txn, gcu_t *ctx) {
  /* При резервировании часть оставшихся страниц может быть использована до полного исчерпания остатка,
   * что также может привести к переработке дополнительных записей GC. Таким образом, на каждой итерации
   * ситуация может существенно меняться, в том числе может потребоваться очистка резерва и повтор всего цикла.
   *
   * Кроме этого, теоретически в GC могут быть очень большие записи (созданные старыми версиями движка и/или
   * при выключенной опции MDBX_ENABLE_BIGFOOT), которые при возврате будут нарезаться на более мелкие куски.
   * В этом случае возвращаемых записей будет больше чем было переработано, поэтому потребуются дополнительные
   * идентификаторы/слоты отсутствующие в GC. */

  // gc_solve_test(txn, ctx);

  tASSERT0(txn, rkl_empty(&txn->wr.gc.reclaimed));
  const size_t amount = pnl_size(txn->wr.repnl);
  if (ctx->return_reserved_hi >= amount) {
    if (unlikely(ctx->dense)) {
      ctx->dense = false;
      NOTICE("%s: out of dense-mode (amount %zu, reserved %zu..%zu)", gc_dbg_prefix(ctx), amount,
             ctx->return_reserved_lo, ctx->return_reserved_hi);
    }
    if (unlikely(amount ? (amount + txn->env->maxgc_large1page < ctx->return_reserved_lo)
                        : (ctx->return_reserved_hi > 3))) {
      /* после резервирования было израсходованно слишком много страниц и получилось слишком много резерва */
      TRACE("%s: reclaimed-list %zu < reversed %zu..%zu, retry", gc_dbg_prefix(ctx), amount, ctx->return_reserved_lo,
            ctx->return_reserved_hi);
      return MDBX_RESULT_TRUE;
    }
    /* резерва достаточно, ничего делать не надо */
    return MDBX_SUCCESS;
  }

  const size_t left_min = amount - ctx->return_reserved_hi;
  const size_t left_max = amount - ctx->return_reserved_lo;
  if (likely(left_min < txn->env->maxgc_large1page && !rkl_empty(&txn->wr.gc.ready4reuse))) {
    /* Есть хотя-бы один слот и весь остаток списка номеров страниц помещается в один кусок.
     * Это самая частая ситуация, просто продолжаем. */
  } else {
    if (likely(rkl_len(&txn->wr.gc.ready4reuse) * ctx->goodchunk >= left_max)) {
      /* Слотов хватает, основная задача делить на куски так, чтобы изменение (уменьшение) кол-ва возвращаемых страниц в
       * процессе резервирования записей в GC не потребовало менять резервирование, т.е. удалять и повторять всё снова.
       */
    } else {
      /* Слотов нет, либо не хватает для нарезки возвращаемых страниц кусками по goodchunk */
      ctx->return_left = left_max;
      int err = gc_search_holes(txn, ctx);
      tASSERT0(txn, (ctx->return_left <= 0) == (err == MDBX_RESULT_TRUE));
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;

      if (!rkl_empty(&ctx->sequel)) {
        err = rkl_merge(&ctx->sequel, &txn->wr.gc.ready4reuse, false);
        if (unlikely(err != MDBX_SUCCESS)) {
          if (err == MDBX_RESULT_TRUE) {
            ERROR("%s/%d: %s", "MDBX_PROBLEM", MDBX_PROBLEM, "unexpected duplicate(s) during rkl-merge");
            err = MDBX_PROBLEM;
          }
          return err;
        }
        rkl_clear(&ctx->sequel);
      }

      if (unlikely(ctx->return_left > 0)) {
        /* Делаем переоценку баланса для кусков предельного размера (по maxgc_large1page, вместо goodchunk). */
        const intptr_t dense_unfit = left_min - rkl_len(&txn->wr.gc.ready4reuse) * txn->env->maxgc_large1page;
        if (dense_unfit > 0) {
          /* Имеющихся идентификаторов НЕ хватит,
           * даже если если их использовать для кусков размером maxgc_large1page вместо goodchunk. */
          if (!ctx->dense) {
            NOTICE("%s: enter to dense-mode (amount %zu, reserved %zu..%zu, slots/ids %zu, left %zu..%zu, unfit %zu)",
                   gc_dbg_prefix(ctx), amount, ctx->return_reserved_lo, ctx->return_reserved_hi,
                   rkl_len(&txn->wr.gc.ready4reuse), left_min, left_max, dense_unfit);
            ctx->dense = true;
          }
          return gc_handle_dense(txn, ctx, left_min, left_max);
        }
      }
      tASSERT0(txn, rkl_empty(&txn->wr.gc.reclaimed));
    }
  }

  /* Максимальный размер куска, который помещается на листовой странице, без выноса на отдельную "overflow" страницу. */
  const size_t chunk_inpage = (txn->env->leaf_nodemax - NODESIZE - sizeof(txnid_t)) / sizeof(pgno_t) - 1;

  /* Размер куска помещающийся на одну отдельную "overflow" страницу, но с небольшим запасом сводобного места. */
  const size_t chunk_good = ctx->goodchunk;

  /* Учитываем резервирование по минимальному размеру кусков (chunk_lo), но резервируем слоты с некоторым запасом
   * (chunk_hi). При этом предполагая что каждый слот может быть заполнен от chunk_lo до chunk_hi, что обеспечивает
   * хорошую амортизацию изменения размера списка возвращаемых страниц, как из-за расходов на создаваемые записи, так и
   * из-за переработки GC. */
  const size_t chunk_lo = (left_min < chunk_inpage) ? left_min : chunk_good;
  /* Куски размером больше chunk_inpage и до maxgc_large1page включительно требуют одной "overflow" страницы.
   * Соответственно требуют одинаковых затрат на обслуживание, а диапазон между chunk_good и maxgc_large1page
   * амортизирует изменения кол-ва списка возвращаемых страниц.
   *
   * Выравниваем размер коротких кусков на 4 (т.е. до 3, с учетом нулевого элемента с длиной),
   * а длинных кусков до maxgc_large1page */
  const size_t chunk_hi = (((left_max + 1) | 3) > chunk_inpage) ? txn->env->maxgc_large1page : ((left_max + 1) | 3);

  TRACE("%s: chunk %zu...%zu, gc-per-ovpage %u", gc_dbg_prefix(ctx), chunk_lo, chunk_hi, txn->env->maxgc_large1page);
  tASSERT0(txn, chunk_lo > 0 && chunk_lo <= chunk_hi && chunk_hi > 1);
  return gc_reserve4return(txn, ctx, chunk_lo, chunk_hi);
}

static int gc_enforce_not_spilled(MDBX_txn *txn, gcu_t *ctx, txnid_t id, MDBX_val *data) {
  while (true) {
#ifndef _MSC_VER
    MDBX_val key = {.iov_base = &id, .iov_len = sizeof(id)};
#else
    /* avoid MSVC crash and/or ICE */
    MDBX_val key;
    key.iov_base = &id;
    key.iov_len = sizeof(id);
#endif

    data->iov_base = nullptr;
    data->iov_len = 0;
    int err = cursor_seek(&ctx->cursor, &key, data, MDBX_SET).err;
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (likely((txn->flags & MDBX_TXN_SPILLS)) == 0)
      break;

    const size_t offset = ptr_dist(data->iov_base, txn->env->dxb_mmap.base);
    if (offset < pgno2bytes(txn->env, txn->geo.first_unallocated)) {
      const page_t *mp = ptr2page(txn->env, data->iov_base);
      tASSERT0(txn, !is_frozen(txn, mp));
      if (likely(!is_spilled(txn, mp))) {
        tASSERT0(txn, is_modifiable(txn, mp));
        break;
      }
      err = is_largepage(mp) ? page_unspill(txn, mp).err : page_touch_unmodifiable(txn, &ctx->cursor, mp);
      tASSERT0(txn, err == MDBX_SUCCESS);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    } else {
      tASSERT0(txn, (txn->flags & MDBX_WRITEMAP) == 0);
      /* we assume that the target data is located on a shadowed page in RAM */
      break;
    }
  }
  return MDBX_SUCCESS;
}

/* Заполняет зарезервированные записи номерами возвращаемых в GC страниц. */
static int gc_fill_returned(MDBX_txn *txn, gcu_t *ctx) {
  tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated - 1));
  tASSERT1(txn, txn_dpl_check(txn));
  MDBX_val data;

  /* Уже есть набор зарезервированных записей GC, id которых собраны в txn->wr.gc.comeback. При этом текущее
   * кол-вол возвращаемых страниц (оставшихся после расходов на резервирование) точно помещается в
   * эти записи и скорее всего с некоторым запасом. Иначе, если резерва недостаточно или избыток
   * резерва неприемлемо велик, то нет другого способа как удалить все созданные записи и повторить
   * всё ещё раз, и дальше этот путь здесь не рассматривается.
   *
   * В большинстве случаев, при резервировании записей переработка GC происходить не будет. Поэтому
   * размер резервированных записей кроме последней будет равен gc_largechunk_preferred_size(),
   * а последней округлённому/выровненному остатку страниц. Однако, в общем случае, может существенно
   * колебаться как размер записей, так и "баланс" отклонения от среднего.
   *
   * Если считать что резерва достаточно и имеющийся избыток допустим, то задача заполнения сводится
   * к распределению излишков резерва по записям с учётом их размера, а далее просто к записи данных.
   * При этом желательно обойтись без каких-то сложных операций типа деления и т.п. */
  const size_t amount = pnl_size(txn->wr.repnl);
  tASSERT0(txn, amount > 0 && amount <= ctx->return_reserved_hi && !rkl_empty(&txn->wr.gc.comeback));
  const size_t slots = rkl_len(&txn->wr.gc.comeback);
  if (likely(slots == 1)) {
    /* самый простой и частый случай */
    txnid_t id = rkl_lowest(&txn->wr.gc.comeback);
    int err = gc_enforce_not_spilled(txn, ctx, id, &data);
    if (likely(err == MDBX_SUCCESS)) {
      pgno_t *const from = MDBX_PNL_BEGIN(txn->wr.repnl), *const to = MDBX_PNL_END(txn->wr.repnl);
      TRACE("%s: fill %zu [ %zu:%" PRIaPGNO "...%zu:%" PRIaPGNO "] @%" PRIaTXN " (%s)", gc_dbg_prefix(ctx),
            pnl_size(txn->wr.repnl), from - txn->wr.repnl, from[0], to - txn->wr.repnl, to[-1], id, "at-once");
      tASSERT0(txn, data.iov_len >= gc_chunk_bytes(pnl_size(txn->wr.repnl)));
      if (unlikely(data.iov_len - gc_chunk_bytes(pnl_size(txn->wr.repnl)) >= txn->env->ps * 2)) {
        NOTICE("too long %s-comeback-reserve @%" PRIaTXN ", have %zu bytes, need %zu bytes", "single", id, data.iov_len,
               gc_chunk_bytes(pnl_size(txn->wr.repnl)));
        return MDBX_RESULT_TRUE;
      }
      /* coverity[var_deref_model] */
      memcpy(data.iov_base, txn->wr.repnl, gc_chunk_bytes(pnl_size(txn->wr.repnl)));
    }
    return err;
  }

  rkl_iter_t iter = rkl_iterator(&txn->wr.gc.comeback, gc_is_lifo(txn));
  size_t surplus = ctx->return_reserved_hi - amount, stored = 0;
  const uint64_t factor = ((uint64_t)surplus << 32) / ctx->return_reserved_hi;
  ratio2digits_buffer_t factor_rough, factor_sharp;
  TRACE("%s: amount %zu, slots %zu, surplus %zu (%zu..%zu), factor %s (%" PRIu64 " >> 32, sharp %s)",
        gc_dbg_prefix(ctx), amount, slots, surplus, ctx->return_reserved_lo, ctx->return_reserved_hi,
        ratio2digits(factor, UINT32_MAX, &factor_rough, 6), factor,
        ratio2digits(surplus, ctx->return_reserved_hi, &factor_sharp, 12));

  do {
    const size_t left = amount - stored;
    tASSERT0(txn, left > 0 && left <= amount);
    txnid_t id = rkl_turn(&iter, gc_is_lifo(txn));
    if (unlikely(!id)) {
      ERROR("reserve depleted (used %zu slots, left %zu loop %u)", rkl_len(&txn->wr.gc.comeback), left, ctx->loop);
      return MDBX_PROBLEM;
    }
    int err = gc_enforce_not_spilled(txn, ctx, id, &data);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    tASSERT0(txn, data.iov_len >= sizeof(pgno_t) * 2);
    const size_t chunk_hi = data.iov_len / sizeof(pgno_t) - 1;
    tASSERT0(txn, chunk_hi >= 2);
    size_t chunk = left;
    if (chunk > chunk_hi) {
      chunk = chunk_hi;
      const size_t left_slots = rkl_left(&iter, gc_is_lifo(txn));
      if (surplus && left_slots) {
        /* Единственный путь выполнения (набор условий) когда нужно распределять избыток резерва. */
        const size_t hole = (size_t)((chunk_hi * factor) >> 32);
        tASSERT0(txn, hole < chunk_hi && hole <= surplus);
        chunk = chunk_hi - hole;
        tASSERT0(txn, chunk > 0 && chunk <= chunk_hi && surplus >= hole);
        tASSERT0(txn, surplus - hole >= (left + surplus - chunk_hi) * factor >> 32);
        if (chunk > 1 && surplus - hole > (left + surplus - chunk_hi) * factor >> 32)
          chunk -= 1;
      }
      tASSERT0(txn, chunk <= chunk_hi && surplus >= chunk_hi - chunk && chunk <= left);
    }
    surplus -= chunk_hi - chunk;

    pgno_t *const dst = data.iov_base;
    pgno_t *const src = MDBX_PNL_BEGIN(txn->wr.repnl) + left - chunk;
    pgno_t *const from = src, *const to = src + chunk;
    TRACE("%s: fill +%zu (surplus %zu) [ %zu:%" PRIaPGNO "...%zu:%" PRIaPGNO "] @%" PRIaTXN " (%s)", gc_dbg_prefix(ctx),
          chunk, chunk_hi - chunk, from - txn->wr.repnl, from[0], to - txn->wr.repnl, to[-1], id, "series");
    TRACE("%s: left %zu, surplus %zu, slots %zu", gc_dbg_prefix(ctx), amount - (stored + chunk), surplus,
          rkl_left(&iter, gc_is_lifo(txn)));
    tASSERT0(txn, chunk > 0 && chunk <= chunk_hi && chunk <= left);
    if (unlikely(data.iov_len - gc_chunk_bytes(chunk) >= txn->env->ps)) {
      NOTICE("too long %s-comeback-reserve @%" PRIaTXN ", have %zu bytes, need %zu bytes", "multi", id, data.iov_len,
             gc_chunk_bytes(chunk));
      return MDBX_RESULT_TRUE;
    }

    /* coverity[var_deref_op] */
    *dst = (pgno_t)chunk;
    memcpy(dst + 1, src, chunk * sizeof(pgno_t));
    stored += chunk;
  } while (stored < amount);
  return MDBX_SUCCESS;
}

int gc_update(MDBX_txn *txn, gcu_t *ctx) {
  TRACE("\n>>> @%" PRIaTXN, txn->txnid);
  /* USDT mdbx:commit__gc_update__begin (txnid, loop): see skynet/probes.md */
  MDBX_DTRACE2(commit__gc_update__begin, (uint64_t)txn->txnid, (uint32_t)ctx->loop);
  MDBX_env *const env = txn->env;
  ctx->cursor.next = txn->cursors[FREE_DBI];
  txn->cursors[FREE_DBI] = &ctx->cursor;
  int err;

  if (unlikely(!txn->env->gc.detent))
    txn_gc_detent(txn);

  if (CHECKS2_ENABLED())
    ENSURE_OBJ(txn, audit_ex(txn, 0, false) == MDBX_SUCCESS);

  /* The txn->wr.repnl[] can grow and shrink during this call.
   * The txn->wr.gc.reclaimed[] can grow, then migrate into txn->wr.gc.ready4reuse and later to txn->wr.gc.comeback[].
   * But page numbers cannot disappear from txn->wr.retired_pages[]. */
retry:
  ctx->loop += !(ctx->prev_first_unallocated > txn->geo.first_unallocated);
  TRACE(">> %sstart, loop %u, gc: txn-rkl %zu, detent %" PRIaTXN, (ctx->loop > 1) ? "re" : "", ctx->loop,
        rkl_len(&txn->wr.gc.reclaimed), txn->env->gc.detent);

  tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated));
  tASSERT1(txn, txn_dpl_check(txn));
  if (unlikely(/* paranoia */ ctx->loop > ((MDBX_DEBUG > 0) ? 12 : 42))) {
    ERROR("txn #%" PRIaTXN " too many loops %u, bailout", txn->txnid, ctx->loop);
    err = MDBX_PROBLEM;
    goto bailout;
  }

  if (unlikely(ctx->prev_first_unallocated > txn->geo.first_unallocated)) {
    err = gc_clean_stored_retired(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;
  }

  ctx->prev_first_unallocated = txn->geo.first_unallocated;
  err = gc_clear_returned(txn, ctx);
  if (unlikely(err != MDBX_SUCCESS))
    goto bailout;

  while (true) {
    /* come back here after each put() in case retired-list changed */
    TRACE("%s", " >> continue");

    tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated));
    err = gc_clear_reclaimed(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;

    tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated));
    tASSERT1(txn, txn_dpl_check(txn));
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(txn, audit_ex(txn, ctx->retired_stored, false) == MDBX_SUCCESS);

    if (txn->wr.loose_pages) {
      /* merge loose pages into the reclaimed- either retired-list */
      err = gc_merge_loose(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err == MDBX_RESULT_TRUE)
          continue;
        goto bailout;
      }
      tASSERT0(txn, txn->wr.loose_pages == 0);
    }

    /* return suitable into unallocated space */
    if (txn_refund(txn)) {
      tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated - 1));
      if (CHECKS2_ENABLED())
        ENSURE_OBJ(txn, audit_ex(txn, ctx->retired_stored, false) == MDBX_SUCCESS);
    }

    if (ctx->retired_stored < pnl_size(txn->wr.retired_pages)) {
      /* store retired-list into GC */
      err = gc_store_retired(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS))
        goto bailout;
      continue;
    }

    tASSERT1(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated - 1));
    tASSERT0(txn, txn->wr.loose_count == 0);
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(txn, audit_ex(txn, ctx->retired_stored, false) == MDBX_SUCCESS);

    if (unlikely(pnl_size(txn->wr.repnl) + env->maxgc_large1page <= ctx->return_reserved_lo) && !ctx->dense) {
      /* после резервирования было израсходованно слишком много страниц и получилось слишком много резерва */
      TRACE("%s: reclaimed-list %zu < reversed %zu, retry", gc_dbg_prefix(ctx), pnl_size(txn->wr.repnl),
            ctx->return_reserved_lo);
      goto retry;
    }

    if (ctx->return_reserved_hi < pnl_size(txn->wr.repnl)) {
      /* верхней границы резерва НЕ хватает, продолжаем резервирование */
      TRACE(">> %s, %zu...%zu, %s %zu", "reserving", ctx->return_reserved_lo, ctx->return_reserved_hi, "return-left",
            pnl_size(txn->wr.repnl) - ctx->return_reserved_hi);
      err = gc_rerere(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err == MDBX_RESULT_TRUE)
          goto retry;
        goto bailout;
      }
      continue;
    }

    if (pnl_size(txn->wr.repnl) > 0) {
      TRACE(">> %s, %s %zu -> %zu...%zu", "filling", "return-reserved", pnl_size(txn->wr.repnl),
            ctx->return_reserved_lo, ctx->return_reserved_hi);
      err = gc_fill_returned(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err == MDBX_RESULT_TRUE)
          goto retry;
        goto bailout;
      }
    }
    break;
  }

  tASSERT0(txn, err == MDBX_SUCCESS);
  if (CHECKS2_ENABLED())
    ENSURE_OBJ(txn, audit_ex(txn, ctx->retired_stored + pnl_size(txn->wr.repnl), true) == MDBX_SUCCESS);
  if (unlikely(txn->wr.loose_count > 0)) {
    DEBUG("** restart: got %zu loose pages", txn->wr.loose_count);
    goto retry;
  }

bailout:
  txn->cursors[FREE_DBI] = ctx->cursor.next;

  pnl_clear(txn->wr.repnl);
#if MDBX_ENABLE_PROFGC
  env->lck->pgops.gc_prof.wloops += (uint32_t)ctx->loop;
#endif /* MDBX_ENABLE_PROFGC */
  TRACE("<<< %u loops, rc = %d\n", ctx->loop, err);
  /* USDT mdbx:commit__gc_update__end (txnid, rc): see skynet/probes.md */
  MDBX_DTRACE2(commit__gc_update__end, (uint64_t)txn->txnid, (int32_t)err);
  return err;
}

#if MDBX_DEBUG_GCU
#pragma pop_macro("LOG_ENABLED")
#endif /* MDBX_DEBUG_GCU */
