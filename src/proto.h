/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

/* Internal prototypes */

MDBX_INTERNAL int MDBX_PRINTF_ARGS(2, 3) bad_page(const page_t *mp, const char *fmt, ...);
MDBX_INTERNAL void MDBX_PRINTF_ARGS(2, 3) poor_page(const page_t *mp, const char *fmt, ...);

/* audit.c */
MDBX_INTERNAL int audit_ex(MDBX_txn *txn, size_t retired_stored, bool dont_filter_gc);

/* mvcc-readers.c */
MDBX_INTERNAL bsr_t mvcc_bind_slot(MDBX_env *env);
MDBX_MAYBE_UNUSED MDBX_INTERNAL pgno_t mvcc_largest_this(MDBX_env *env, pgno_t largest);
MDBX_INTERNAL pgno_t mvcc_snapshot_largest(const MDBX_env *env, pgno_t last_used_page);
MDBX_INTERNAL int mvcc_cleanup_dead(MDBX_env *env, int rlocked, int *dead);

struct gc_reclaiming_obstacle;
MDBX_INTERNAL bool mvcc_kick_laggards(MDBX_txn *txn, const txnid_t laggard,
                                      struct gc_reclaiming_obstacle *optional_obstacle);

typedef struct rw_oldest_readed_shapshot_into {
  txnid_t oldest_txnid, steady_txnid;
} orsi_rw_t;
MDBX_INTERNAL orsi_rw_t mvcc_shapshot_oldest_rw(const MDBX_txn *const txn);

typedef struct ro_oldest_readed_shapshot_into {
  txnid_t oldest_txnid, thisprocess_oldest_txnid;
  txnid_t recent_txnid;
  size_t nreaders;
} orsi_ro_t;
MDBX_INTERNAL orsi_ro_t mvcc_shapshot_oldest_ro(const MDBX_txn *const txn, const bool need_thisprocess_oldest);

/* dxb.c */
MDBX_INTERNAL int dxb_setup(MDBX_env *env, const int lck_rc, const mdbx_mode_t mode_bits);
MDBX_INTERNAL int __must_check_result dxb_read_header(MDBX_env *env, meta_t *meta, const int lck_exclusive,
                                                      const mdbx_mode_t mode_bits);
enum resize_mode { implicit_grow, impilict_shrink, explicit_resize };
MDBX_INTERNAL int __must_check_result dxb_resize(MDBX_env *const env, const pgno_t used_pgno, const pgno_t size_pgno,
                                                 pgno_t limit_pgno, const enum resize_mode mode);
MDBX_INTERNAL int dxb_set_readahead(const MDBX_env *env, const pgno_t edge, const bool enable, const bool force_whole);
MDBX_INTERNAL int dxb_msync(const MDBX_env *env, size_t length_pages, enum osal_syncmode_bits mode_bits);
MDBX_INTERNAL int dxb_fsync(const MDBX_env *env, enum osal_syncmode_bits mode_bits);
MDBX_INTERNAL int __must_check_result dxb_sync_locked(MDBX_env *env, unsigned flags, meta_t *const pending,
                                                      troika_t *const troika);
#if defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__)
MDBX_INTERNAL void dxb_sanitize_tail(MDBX_env *env, MDBX_txn *txn);
#else
MDBX_MAYBE_UNUSED static inline void dxb_sanitize_tail(MDBX_env *env, MDBX_txn *txn) {
  (void)env;
  (void)txn;
}
#endif /* ENABLE_MEMCHECK || __SANITIZE_ADDRESS__ */

struct commit_timestamp {
  uint64_t start, prep, gc, audit, write, sync, gc_cpu;
};
MDBX_INTERNAL pgop_stat_t *txn_latency_gcprof(const MDBX_env *env, MDBX_commit_latency *latency);

MDBX_INTERNAL bool txn_refund(MDBX_txn *txn);
MDBX_INTERNAL bool txn_gc_detent(const MDBX_txn *const txn);
MDBX_INTERNAL int txn_check_badbits_parked(const MDBX_txn *txn, int bad_bits);
MDBX_INTERNAL void txn_done_cursors(MDBX_txn *txn);
MDBX_INTERNAL int txn_shadow_cursors(const MDBX_txn *parent, const size_t dbi);

MDBX_INTERNAL MDBX_txn *txn_alloc(const unsigned flags, MDBX_env *env);
MDBX_INTERNAL int txn_abort(MDBX_txn *txn, MDBX_commit_latency *latency);
MDBX_INTERNAL int txn_commit(MDBX_txn *txn, MDBX_commit_latency *latency, struct commit_timestamp *ts);
#if !(defined(_WIN32) || defined(_WIN64))
MDBX_INTERNAL void txn_abort_after_resurrect(MDBX_txn *txn);
#endif /* Windows */
MDBX_INTERNAL int txn_setup_primal(MDBX_txn *txn);

MDBX_INTERNAL int txn_nested_create(MDBX_txn *parent, bool readonly);
MDBX_INTERNAL int txn_nested_abort(MDBX_txn *txn);
MDBX_INTERNAL int txn_nested_commit(MDBX_txn *txn, struct commit_timestamp *ts);
MDBX_INTERNAL int txn_nested_checkpoint(MDBX_txn *txn, struct commit_timestamp *ts);
MDBX_INTERNAL int txn_nested_rollback(MDBX_txn *txn);
MDBX_INTERNAL MDBX_txn *txn_nested_fakero_begin(MDBX_txn *parent);
MDBX_INTERNAL int txn_nested_fakero_end(MDBX_txn *txn);

MDBX_INTERNAL MDBX_txn *txn_basal_create(const size_t max_dbi);
MDBX_INTERNAL void txn_basal_destroy(MDBX_txn *txn);
MDBX_INTERNAL int txn_basal_start(MDBX_txn *txn, unsigned flags);
MDBX_INTERNAL int txn_basal_commit(MDBX_txn *txn, struct commit_timestamp *ts);
MDBX_INTERNAL int txn_basal_end(MDBX_txn *txn, bool unlock);
MDBX_INTERNAL int txn_basal_checkpoint(MDBX_txn *txn, MDBX_txn_flags_t weakening_durability,
                                       struct commit_timestamp *ts);
MDBX_INTERNAL int txn_basal_rollback(MDBX_txn *txn);
MDBX_INTERNAL int txn_basal_update_tbl_roots(MDBX_txn *txn);

MDBX_INTERNAL int txn_ro_park(MDBX_txn *txn, bool autounpark);
MDBX_INTERNAL int txn_ro_unpark(MDBX_txn *txn);
MDBX_INTERNAL int txn_ro_start(MDBX_txn *txn, bool prepare_only);
MDBX_INTERNAL int txn_ro_clone(const MDBX_txn *const source, MDBX_txn *const clone);
MDBX_INTERNAL int txn_ro_reset(MDBX_txn *txn);
MDBX_INTERNAL void txn_ro_free(MDBX_txn *txn);

MDBX_INTERNAL int env_open(MDBX_env *env, mdbx_mode_t mode);
MDBX_INTERNAL int env_info(const MDBX_env *env, const MDBX_txn *txn, MDBX_envinfo *out, troika_t *troika);
MDBX_INTERNAL int env_sync(MDBX_env *env, bool force, bool nonblock);
MDBX_INTERNAL int env_close(MDBX_env *env, bool resurrect_after_fork);
MDBX_INTERNAL MDBX_txn *env_owned_wrtxn(const MDBX_env *env);
MDBX_INTERNAL int __must_check_result env_page_auxbuffer(MDBX_env *env);
MDBX_INTERNAL unsigned env_setup_pagesize(MDBX_env *env, const size_t pagesize);
MDBX_INTERNAL bool env_is_page_incore(MDBX_env *const env, pgno_t pgno);
MDBX_INTERNAL void env_clear_incore_cache(const MDBX_env *const env);

MDBX_INTERNAL void env_options_init(MDBX_env *env);
MDBX_INTERNAL void env_options_adjust_defaults(MDBX_env *env);
MDBX_INTERNAL void env_options_adjust_dp_limit(MDBX_env *env);
MDBX_INTERNAL pgno_t default_dp_limit(const MDBX_env *env);

MDBX_INTERNAL int __must_check_result tree_deepen_edge(MDBX_cursor *mc, int flags);
MDBX_INTERNAL int tree_deepen_lowest(MDBX_cursor *mc);
MDBX_INTERNAL intptr_t tree_diff_level(const MDBX_cursor *left, const MDBX_cursor *right);
MDBX_INTERNAL size_t tree_search_branch_configure(const MDBX_cursor *mc, const MDBX_val *key);
MDBX_INTERNAL sfr_t tree_search_foliage_configure(MDBX_cursor *mc, const MDBX_val *key);

enum page_search_flags {
  Z_MODIFY = 1,
  Z_ROOTONLY = 2,
  Z_FIRST = 4,
  Z_LAST = 8,
};
MDBX_INTERNAL int __must_check_result tree_search(MDBX_cursor *mc, const MDBX_val *key, int flags);

static inline size_t tree_search_branch(const MDBX_cursor *mc, const MDBX_val *key) {
  return mc->clc->k.search_branch(mc, key);
}

static inline sfr_t tree_search_foliage(MDBX_cursor *mc, const MDBX_val *key) {
  return mc->clc->k.search_foliage(mc, key);
}

#if MDBX_ENABLE_BUNCHES_REMOVAL
MDBX_INTERNAL int tree_cutoff_twig(MDBX_cursor *mc, const pgno_t pgno, size_t deep, txnid_t parent_txnid,
                                   const bool whole_tree);
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
MDBX_INTERNAL int tree_curoff_range(MDBX_cursor *begin, MDBX_cursor *end, bool end_including);
MDBX_INTERNAL int tree_drop(MDBX_cursor *mc);
MDBX_INTERNAL int __must_check_result tree_rebalance(MDBX_cursor *mc);
MDBX_INTERNAL int __must_check_result tree_propagate_key(MDBX_cursor *mc, const MDBX_val *key);
MDBX_INTERNAL void recalculate_merge_thresholds(MDBX_env *env);
MDBX_INTERNAL void recalculate_subpage_thresholds(MDBX_env *env);

MDBX_INTERNAL int __must_check_result tbl_fetch(MDBX_txn *txn, MDBX_cursor *mc, size_t dbi, const MDBX_val *name,
                                                unsigned wanna_flags);
MDBX_INTERNAL int __must_check_result tbl_create(MDBX_txn *txn, MDBX_cursor *mc, size_t slot, const MDBX_val *name,
                                                 unsigned db_flags);
MDBX_INTERNAL int __must_check_result tbl_setup(const MDBX_env *env, volatile kvx_t *const kvx, const tree_t *const db);
MDBX_INTERNAL int __must_check_result tbl_refresh(MDBX_txn *txn, size_t dbi);
MDBX_INTERNAL int __must_check_result tbl_purge(MDBX_cursor *mc);
MDBX_INTERNAL int __must_check_result tbl_stat_summary(const MDBX_txn *txn, MDBX_stat *st);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL txnid_t tbl_root_txnid(const MDBX_txn *txn, const size_t dbi);

/* coherency.c */
MDBX_INTERNAL bool coherency_check_meta(const MDBX_env *env, const volatile meta_t *meta, bool report);
MDBX_INTERNAL int coherency_fetch_head(MDBX_txn *txn, const meta_ptr_t head, uint64_t *timestamp);
MDBX_INTERNAL int coherency_check_written(const MDBX_env *env, const txnid_t txnid, const volatile meta_t *meta,
                                          const intptr_t pgno, uint64_t *timestamp);
MDBX_INTERNAL int coherency_timeout(uint64_t *timestamp, intptr_t pgno, const MDBX_env *env);

/* histogram.c */
#define HISTOGRAM_LE0 1
MDBX_INTERNAL void histogram_acc_ex(const size_t value, struct MDBX_chk_histogram *histogram, unsigned options);
MDBX_MAYBE_UNUSED static inline void histogram_acc(const size_t value, struct MDBX_chk_histogram *histogram) {
  histogram_acc_ex(value, histogram, 0);
}
MDBX_INTERNAL MDBX_chk_line_t *histogram_dist(MDBX_chk_line_t *line, const struct MDBX_chk_histogram *histogram,
                                              const char *prefix, const char *first, bool amount);
MDBX_INTERNAL MDBX_chk_line_t *histogram_print(MDBX_chk_scope_t *scope, MDBX_chk_line_t *line,
                                               const struct MDBX_chk_histogram *histogram, const char *prefix,
                                               const char *first, bool amount);

/* print.c */
MDBX_INTERNAL void chk_line_end(MDBX_chk_line_t *line);
MDBX_INTERNAL __must_check_result MDBX_chk_line_t *chk_line_begin(MDBX_chk_scope_t *const scope,
                                                                  enum MDBX_chk_severity severity);
MDBX_INTERNAL MDBX_chk_line_t *chk_line_feed(MDBX_chk_line_t *line);
MDBX_INTERNAL MDBX_chk_line_t *chk_flush(MDBX_chk_line_t *line);
MDBX_INTERNAL size_t chk_print_wanna(MDBX_chk_line_t *line, size_t need);
MDBX_INTERNAL MDBX_chk_line_t *chk_puts(MDBX_chk_line_t *line, const char *str);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_va(MDBX_chk_line_t *line, const char *fmt, va_list args);
MDBX_INTERNAL MDBX_chk_line_t *MDBX_PRINTF_ARGS(2, 3) chk_print(MDBX_chk_line_t *line, const char *fmt, ...);
MDBX_INTERNAL void chk_println_va(MDBX_chk_scope_t *const scope, enum MDBX_chk_severity severity, const char *fmt,
                                  va_list args);
MDBX_INTERNAL void chk_println(MDBX_chk_scope_t *const scope, enum MDBX_chk_severity severity, const char *fmt, ...);
