/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file chk-internal.h — internal declarations shared between the parts of
/// chk.c (chk-report.c / chk-walk.c / chk.c umbrella). Included via the anchor
/// in internals.h (inside #ifndef __cplusplus), so it is not visible to the
/// C++ API TU.

#pragma once

typedef struct MDBX_chk_internal {
  MDBX_chk_context_t *usr;
  const struct MDBX_chk_callbacks *cb;
  uint64_t monotime_timeout;

  size_t *problem_counter;
  uint8_t flags;
  bool got_break;
  bool write_locked;
  uint8_t scope_depth;
  pgno_t last_nested_root;

  MDBX_chk_table_t table_gc, table_main;
  int16_t *pagemap;
  MDBX_chk_table_t *last_lookup;
  MDBX_chk_scope_t scope_stack[12];
  MDBX_chk_table_t *table[MDBX_MAX_DBI + CORE_DBS];

  MDBX_envinfo envinfo;
  troika_t troika;
  MDBX_val v2a_buf;
} MDBX_chk_internal_t;

MDBX_INTERNAL int chk_error_rc(MDBX_chk_scope_t *const scope, int err, const char *subj);
MDBX_INTERNAL void MDBX_PRINTF_ARGS(5, 6)
    chk_object_issue(MDBX_chk_scope_t *const scope, const char *object, uint64_t entry_number, const char *caption,
                     const char *extra_fmt, ...);
MDBX_INTERNAL void MDBX_PRINTF_ARGS(2, 3) chk_scope_issue(MDBX_chk_scope_t *const scope, const char *fmt, ...);
MDBX_INTERNAL int chk_scope_end(MDBX_chk_internal_t *chk, int err);
MDBX_INTERNAL int MDBX_PRINTF_ARGS(6, 7)
    chk_scope_begin(MDBX_chk_internal_t *chk, int verbosity_adjustment, enum MDBX_chk_stage stage, const void *object,
                    size_t *problems, const char *fmt, ...);
MDBX_INTERNAL int chk_scope_restore(MDBX_chk_scope_t *const target, int err);
MDBX_INTERNAL MDBX_chk_scope_t *MDBX_PRINTF_ARGS(3, 4)
    chk_scope_push(MDBX_chk_scope_t *const scope, int verbosity_adjustment, const char *fmt, ...);
MDBX_INTERNAL const char *chk_v2a(MDBX_chk_internal_t *chk, const MDBX_val *val);
MDBX_INTERNAL int chk_check_break(MDBX_chk_scope_t *const scope);
typedef int(chk_kv_visitor)(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                            const MDBX_val *key, const MDBX_val *data);
MDBX_INTERNAL int chk_tree(MDBX_chk_scope_t *const scope);
MDBX_INTERNAL int chk_handle_kv(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                                const MDBX_val *key, const MDBX_val *data);
MDBX_INTERNAL int chk_handle_gc(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                                const MDBX_val *key, const MDBX_val *data);
MDBX_INTERNAL int chk_db(MDBX_chk_scope_t *const scope, MDBX_dbi dbi, MDBX_chk_table_t *tbl, chk_kv_visitor *handler);
MDBX_INTERNAL int chk_get_tbl(MDBX_chk_scope_t *const scope, const walk_tbl_t *in, MDBX_chk_table_t **out);

MDBX_INTERNAL MDBX_chk_line_t *chk_print_size(MDBX_chk_line_t *line, const char *prefix, const uint64_t value,
                                              const char *suffix);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_ratio(MDBX_chk_line_t *line, size_t numerator, size_t denominator,
                                               unsigned precision);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_percent(MDBX_chk_line_t *line, const char *triplet, size_t value, size_t whole,
                                                 const char *unit);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_pages_percent(MDBX_chk_line_t *line, const char *triplet, size_t pages,
                                                       size_t whole);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_bytes_percent(MDBX_chk_line_t *line, const char *triplet, size_t pages,
                                                       size_t whole);
MDBX_INTERNAL MDBX_chk_line_t *chk_print_pages_percent_bb(MDBX_chk_line_t *line, const char *prefix, size_t pages,
                                                          size_t backed, size_t boundary);