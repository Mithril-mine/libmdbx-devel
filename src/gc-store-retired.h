/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/** \file gc-store-retired.h — сохранение retired-списка в GC: gc_store_retired() с нарезкой BIGFOOT-кусков
 * (извлечено из gc-put.c, 2026-11, кампания верификации). Требует internals.h (MDBX_txn/cursor, gcu_t из gc.h,
 * pnl.h, logging). Хуки gc_prepare_stockpile4retired()/gc_clean_stored_retired() объявлены здесь и
 * определены в gc-put.c.
 *
 * Включается дважды в одной единице трансляции alloy-сборки: gc-put.c получает реальный
 * экземпляр, а tests/api.c — инструментированный (каллеи и вход заменяются макросами
 * на tst_*-стабы перед включением). Поэтому здесь НЕТ #pragma once: каждый включающий
 * определяет свои макросы-переименования до вставки содержимого. */

#include "internals.h"

MDBX_NOTHROW_PURE_FUNCTION static inline bool gc_is_lifo(const MDBX_txn *txn) {
  return (txn->env->flags & MDBX_LIFORECLAIM) != 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline const char *gc_dbg_prefix(const gcu_t *ctx) {
  return gc_is_lifo(ctx->cursor.txn) ? "    lifo" : "    fifo";
}

MDBX_INTERNAL int gc_prepare_stockpile4retired(MDBX_txn *txn, gcu_t *ctx);
MDBX_INTERNAL int gc_clean_stored_retired(MDBX_txn *txn, gcu_t *ctx);

MDBX_MAYBE_UNUSED static int gc_store_retired(MDBX_txn *txn, gcu_t *ctx) {
  int err;
  MDBX_val key, data;

#if MDBX_ENABLE_BIGFOOT
  size_t retired_before;
  bool should_retry;
  do {
    if (ctx->bigfoot > txn->txnid) {
      err = gc_clean_stored_retired(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      tASSERT0(txn, ctx->bigfoot <= txn->txnid);
    }

    err = gc_prepare_stockpile4retired(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    pnl_sort(txn->wr.retired_pages, txn->geo.first_unallocated);
    retired_before = pnl_size(txn->wr.retired_pages);
    should_retry = false;
    ctx->retired_stored = 0;
    ctx->bigfoot = txn->txnid;
    do {
      if (ctx->retired_stored) {
        err = gc_prepare_stockpile4retired(txn, ctx);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      key.iov_len = sizeof(txnid_t);
      key.iov_base = &ctx->bigfoot;
      const size_t left_before = retired_before - ctx->retired_stored;
      const size_t chunk_hi = ((left_before | 3) > ctx->goodchunk && ctx->bigfoot < (MAX_TXNID - UINT32_MAX))
                                  ? ctx->goodchunk
                                  : (left_before | 3);
      data.iov_len = gc_chunk_bytes(chunk_hi);
      /* Ложные предупреждения coverity возникают из-за поддержки вставки массивов значений в режиме MDBX_MULTIPLE. */
      /* coverity[OVERRUN] */
      err = cursor_put(&ctx->cursor, &key, &data, MDBX_RESERVE);
      if (unlikely(err != MDBX_SUCCESS))
        return err;

#if MDBX_DEBUG && (defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__))
      /* Для предотвращения предупреждения Valgrind из mdbx_dump_val() вызванное через макрос DVAL_DEBUG() на выходе из
       * cursor_seek(MDBX_SET), которая вызывается как выше в цикле очистки, так и ниже в цикле заполнения
       * зарезервированных элементов. */
      memset(data.iov_base, 0xBB, data.iov_len);
#endif /* MDBX_DEBUG && (ENABLE_MEMCHECK || __SANITIZE_ADDRESS__) */

      const size_t retired_after = pnl_size(txn->wr.retired_pages);
      const size_t left_after = retired_after - ctx->retired_stored;
      const size_t chunk = (left_after < chunk_hi) ? left_after : chunk_hi;
      should_retry = retired_before != retired_after;
      if (likely(!should_retry)) {
        const size_t at = (gc_is_lifo(txn) == MDBX_PNL_ASCENDING) ? left_before - chunk : ctx->retired_stored;
        pgno_t *const begin = txn->wr.retired_pages + at;
        /* MDBX_PNL_ASCENDING == false && LIFO == false:
         *  - the larger pgno is at the beginning of retired list
         *    and should be placed with the larger txnid.
         * MDBX_PNL_ASCENDING == true && LIFO == true:
         *  - the larger pgno is at the ending of retired list
         *    and should be placed with the smaller txnid. */
        const pgno_t save = *begin;
        *begin = (pgno_t)chunk;
        memcpy(data.iov_base, begin, data.iov_len);
        *begin = save;
        TRACE("%s: put-retired/bigfoot @ %" PRIaTXN " (slice #%u) #%zu [%zu..%zu] of %zu", gc_dbg_prefix(ctx),
              ctx->bigfoot, (unsigned)(ctx->bigfoot - txn->txnid), chunk, at, at + chunk, retired_before);
      }
      ctx->retired_stored += chunk;
    } while (ctx->retired_stored < pnl_size(txn->wr.retired_pages) && (++ctx->bigfoot, true));
  } while (unlikely(should_retry));
#else
  /* Write to last page of GC */
  key.iov_len = sizeof(txnid_t);
  key.iov_base = &txn->txnid;
  do {
    err = gc_prepare_stockpile4retired(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    data.iov_len = MDBX_PNL_SIZEOF(txn->wr.retired_pages);
    err = cursor_put(&ctx->cursor, &key, &data, MDBX_RESERVE);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

#if MDBX_DEBUG && (defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__))
    /* Для предотвращения предупреждения Valgrind из mdbx_dump_val()
     * вызванное через макрос DVAL_DEBUG() на выходе
     * из cursor_seek(MDBX_SET), которая вызывается как выше в цикле
     * очистки, так и ниже в цикле заполнения зарезервированных элементов. */
    memset(data.iov_base, 0xBB, data.iov_len);
#endif /* MDBX_DEBUG && (ENABLE_MEMCHECK || __SANITIZE_ADDRESS__) */

    /* Retry if wr.retired_pages[] grew during the Put() */
  } while (data.iov_len < MDBX_PNL_SIZEOF(txn->wr.retired_pages));

  ctx->retired_stored = pnl_size(txn->wr.retired_pages);
  pnl_sort(txn->wr.retired_pages, txn->geo.first_unallocated);
  tASSERT0(txn, data.iov_len == MDBX_PNL_SIZEOF(txn->wr.retired_pages));
  memcpy(data.iov_base, txn->wr.retired_pages, data.iov_len);

  TRACE("%s: put-retired #%zu @ %" PRIaTXN, gc_dbg_prefix(ctx), ctx->retired_stored, txn->txnid);
#endif /* MDBX_ENABLE_BIGFOOT */
  if (MDBX_DEBUG_GCU < 2 && LOG_ENABLED(MDBX_LOG_EXTRA)) {
    size_t i = ctx->retired_stored;
    DEBUG_EXTRA("txn %" PRIaTXN " root %" PRIaPGNO " num %zu, retired-PNL", txn->txnid, txn->dbs[FREE_DBI].root, i);
    for (; i; i--)
      DEBUG_EXTRA_PRINT(" %" PRIaPGNO, txn->wr.retired_pages[i]);
    DEBUG_EXTRA_PRINT("%s\n", ".");
  }
  return MDBX_SUCCESS;
}
