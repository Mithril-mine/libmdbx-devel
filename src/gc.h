/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

struct gc_reclaiming_obstacle {
  mdbx_pid_t pid;
  mdbx_tid_t tid;
  txnid_t txnid;
};

/* Гистограмма решения нарезки фрагментов для ситуации нехватки идентификаторов/слотов. */
typedef struct gc_dense_histogram {
  /* Размер массива одновременно задаёт максимальный размер последовательностей,
   * с которыми решается задача распределения.
   *
   * Использование длинных последовательностей контрпродуктивно, так как такие последовательности будут
   * создавать/воспроизводить/повторять аналогичные затруднения при последующей переработке. Однако,
   * в редких ситуациях это может быть единственным выходом. */
  unsigned end;
  pgno_t array[31];
} gc_dense_histogram_t;

typedef struct gc_update_context {
  unsigned loop;
  unsigned goodchunk;
  bool dense;
  pgno_t prev_first_unallocated;
  size_t retired_stored;
  size_t return_reserved_lo, return_reserved_hi;
  txnid_t gc_first;
  intptr_t return_left;
#if MDBX_DEBUG_GCU
  struct {
    txnid_t prev;
    unsigned n;
  } dbg;
#endif /* MDBX_DEBUG_GCU */
  rkl_t sequel;
#if MDBX_ENABLE_BIGFOOT
  txnid_t bigfoot;
#endif /* MDBX_ENABLE_BIGFOOT */
  union {
    MDBX_cursor cursor;
    cursor_couple_t couple;
  };
  gc_dense_histogram_t dense_histogram;
} gcu_t;

MDBX_INTERNAL int gc_put_init(MDBX_txn *txn, gcu_t *ctx);
MDBX_INTERNAL void gc_put_destroy(gcu_t *ctx);

#define ALLOC_DEFAULT 0     /* штатное/обычное выделение страниц */
#define ALLOC_UNIMPORTANT 1 /* запрос неважен, невозможность выделения не приведет к ошибке транзакции */
#define ALLOC_RESERVE 2     /* подготовка резерва для обновления GC, без аллокации */
#define ALLOC_COALESCE 4    /* внутреннее состояние/флажок */
#define ALLOC_SHOULD_SCAN 8 /* внутреннее состояние/флажок */
#define ALLOC_LIFO 16       /* внутреннее состояние/флажок */
/* Экспериментально: получить серию страниц точно запрошенной длины, не
 * разрывая более длинные серии. Пока этот бит не выставляется ни одним
 * вызовом (то есть ветки не исполняются), но код остаётся компилируемым и
 * проверяется white-box тестами (tests/ut/details/details_scan4seq.c). */
#define ALLOC_EXACTLY 32

MDBX_INTERNAL pgr_t gc_alloc_ex(const MDBX_cursor *const mc, const size_t num, uint8_t flags);

MDBX_INTERNAL pgr_t gc_alloc_single(const MDBX_cursor *const mc);
MDBX_INTERNAL int gc_update(MDBX_txn *txn, gcu_t *ctx);

MDBX_NOTHROW_PURE_FUNCTION static inline size_t gc_stockpile(const MDBX_txn *txn) {
  return pnl_size(txn->wr.repnl) + txn->wr.loose_count;
}

MDBX_NOTHROW_PURE_FUNCTION static inline size_t gc_chunk_bytes(const size_t chunk) {
  return (chunk + 1) * sizeof(pgno_t);
}

MDBX_INTERNAL bool gc_repnl_has_span(const MDBX_txn *txn, const size_t num);
MDBX_INTERNAL pgno_t gc_repnl_get_sequence(MDBX_txn *txn, const size_t num, uint8_t flags);
MDBX_INTERNAL pgno_t gc_repnl_get_single(MDBX_txn *txn);

static inline bool gc_is_reclaimed(const MDBX_txn *txn, const txnid_t id) {
  return rkl_contain(&txn->wr.gc.reclaimed, id) || rkl_contain(&txn->wr.gc.comeback, id);
}

MDBX_NOTHROW_PURE_FUNCTION static inline bool gc_may_clean_reclaimed(const MDBX_txn *txn) {
  const size_t threshold = txn->dbs[FREE_DBI].height * 2u + 3u;
  return pnl_size(txn->wr.repnl) + txn->wr.loose_count + (txn->geo.end_pgno - txn->geo.first_unallocated) > threshold &&
         txn->wr.dirtyroom + txn->wr.loose_count > threshold;
}

static inline txnid_t txnid_min(txnid_t a, txnid_t b) { return (a < b) ? a : b; }

static inline txnid_t txnid_max(txnid_t a, txnid_t b) { return (a > b) ? a : b; }

MDBX_INTERNAL MDBX_cursor *gc_cursor_init(MDBX_txn *txn);
MDBX_INTERNAL int gc_merge_loose(MDBX_txn *txn, gcu_t *ctx);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL const char *gc_check_keylen(size_t const key_len);
MDBX_INTERNAL const char *gc_check_rowdata(const MDBX_txn *const txn, const MDBX_val data);

typedef struct gc_pnl_result {
  const_pnl_t pnl;
  int err;
  const char *reason;
} glr_t;
MDBX_INTERNAL glr_t gc_row_pnl(const MDBX_txn *const txn, const MDBX_val data);

/* What made the walk of a defragmentation cycle stumble. */
typedef enum defrag_stumble_kind {
  /* A page that the walk can neither move nor pass: one retired in this cycle, or one held by a reading transaction.
   * Such a page clears by itself; if it does not, the walk is stuck. */
  defrag_stumble_obstacle,
  /* No free page below the goal edge for the moving page and for the copies of its parents: the structure of the trees
   * allows no more single pages to be moved as long as places are taken below the goal edge only. */
  defrag_stumble_no_place,
  /* No free pages at all for the moving page and for the copies of its parents: the structure of the trees allows no
   * more single pages to be moved. */
  defrag_stumble_no_stock,
  /* No run of adjacent free pages for a large page below the goal edge. */
  defrag_stumble_no_span
} defrag_stumble_kind_t;

/* What a cycle, or the cycles from one state to its repeat, stumbled on: the kinds of defrag_stumble_kind_t as bits,
 * with the one kind that depends on the moment split in two. */
enum defrag_stumbles {
  defrag_stumbles_obstacle = 1 << defrag_stumble_obstacle,
  defrag_stumbles_no_place = 1 << defrag_stumble_no_place,
  defrag_stumbles_no_stock = 1 << defrag_stumble_no_stock,
  /* No run for a large page, and none is made for it. */
  defrag_stumbles_no_span = 1 << defrag_stumble_no_span,
  /* No run for a large page, although one is already made for it. It takes the highest bit of the byte, apart from the
   * kinds, so that a new kind of stumble does not collide with it. */
  defrag_stumbles_no_span_reserved = 0x80,
  /* What holds the pages is a limit of the trees, and what places between the goal edge and the pages can cure. */
  defrag_stumbles_limit = defrag_stumbles_no_place | defrag_stumbles_no_stock | defrag_stumbles_no_span,
  defrag_stumbles_between_edges = defrag_stumbles_no_place | defrag_stumbles_no_span
};

STATIC_ASSERT(defrag_stumble_no_span < 7);

typedef struct defract_context {
  MDBX_txn *txn;
  dml_t *arcs;
  pnl_t lp_reserve;
  pnl_t temp;
  pgno_t cycle_preprogress;
  pgno_t cycle_pages_scheduled;
  pgno_t cycle_pages_moved;
  /* The cycle reserved a run of free pages for a large page that had none: the page takes the run in the next cycle. */
  bool cycle_span_reserved;
  pgno_t stumble_pgno, stumble_span, lp_backlog;
  pgno_t remapped_edge;
  pgno_t defrag_edge;
  pgno_t retreat_edge;
  pgno_t move_batch_size;
  size_t payload_items;
  size_t progress_counter;
  uint8_t stopping_reasons;
  /* What made the walk stumble at stumble_pgno. */
  uint8_t stumble_kind;
  bool completed;
  bool batch_adaptive;
  /* The meta-pages held a steady checkpoint at the start of the call: the datafile is truncated only at the end, after
   * a steady checkpoint that covers the new end, and the call leaves a steady checkpoint behind. */
  bool steady_at_start;
  /* The cycles without progress found no place below the goal edge for their pages: the pages also take places
   * between the goal edge and themselves. */
  bool in_region;
  unsigned cycle;
  size_t payload_pages;
  pgno_t largepage_max, largepage_amountleft, largepage_count;
  pgno_t summary_depth;
  void *user_ctx;
  MDBX_defrag_notify_func user_callback;
  size_t total_pages_moved;
  /* The shortest datafile the cycles started from, and the cycles in a row that started from no shorter one: pages that
   * move from one place to another and back do not cut the end of the data, and a state the defragmentation has already
   * had means the cycles from then on would repeat without end. What the cycles without progress stumbled on tells a
   * limit of the trees from an obstacle. */
  pgno_t least_allocated;
  uint8_t no_progress;
  /* The cycles of that run that scheduled nothing: a cycle that moved nothing changed nothing, so two of them in a row
   * are enough. */
  uint8_t idle_cycles;
  uint8_t no_progress_stumbles;
  /* What the current cycle stumbled on. */
  uint8_t cycle_stumbles;

  pgno_t walk_stack[32];
  pgno_t walk_cutoff;
  pgno_t notify_watchmask;

#if MDBX_CHECKING > 1
  pnl_t repnl_clone;
#endif /* MDBX_CHECKING > 1 */
  pgno_t defrag_atleast, defrag_enough, before_defrag, last_allocated;
  /* The residual of pages that the caller accepts. */
  size_t acceptable_backlash;
  /* The pages at the end of the database that the cycle vacated. The commit stores them in the GC, and the next cycle
   * loads them and cuts them off, although the residual without them is within what the caller accepts. */
  pgno_t tail_in_gc;
  pgno_t gc_tree_pages, gc_retained_pages;
  uint64_t start_timestamp;
  uint64_t cycle_started_timestamp;
  uint64_t wallclock_atleast;
  uint64_t wallclock_detent;
  uint64_t cycle_initial_score;

  txnid_t stopor;
  struct gc_reclaiming_obstacle gc_obstacle;
  struct cache_item {
    pgno_t pgno;
    unsigned cost;
  } cache_cost[64];
} dfc_t;

MDBX_INTERNAL int defrag_init(dfc_t *dfc, MDBX_txn *txn, size_t defrag_atleast_pages,
                              size_t spend_atleast_wallclock_16dot16, size_t defrag_enough_pages,
                              size_t limit_spend_wallclock_16dot16, intptr_t preferred_move_batch_size);
MDBX_INTERNAL void defrag_destroy(dfc_t *dfc);
MDBX_INTERNAL int defrag_cycle(dfc_t *dfc);
MDBX_INTERNAL void defrag_milestone(dfc_t *dfc);

MDBX_MAYBE_UNUSED static inline bool defrag_discontinued(const dfc_t *dfc) {
  return dfc->stopping_reasons >= MDBX_defrag_discontinued;
}

MDBX_MAYBE_UNUSED static inline bool defrag_aborted(const dfc_t *dfc) {
  return dfc->stopping_reasons >= MDBX_defrag_aborted;
}

MDBX_INTERNAL uint64_t defrag_score(dfc_t *dfc, size_t allocated_pages);
MDBX_INTERNAL uint64_t defrag_result(dfc_t *dfc, MDBX_defrag_result_t *out, uint64_t now_cache);
