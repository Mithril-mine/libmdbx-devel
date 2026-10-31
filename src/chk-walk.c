/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file chk-walk.c — page-walking callbacks and per-page/key-value checks of mdbx_env_chk().

#include "internals.h"

__cold static int chk_pgvisitor(const size_t pgno, const unsigned npages, void *const ctx, const unsigned deep,
                                const walk_tbl_t *tbl_info, const size_t page_size, const page_type_t pagetype,
                                const txnid_t page_txnid, const MDBX_error_t page_err, const size_t nentries,
                                const size_t payload_bytes, const size_t header_bytes, const size_t unused_bytes,
                                const size_t parent_pgno) {
  MDBX_chk_scope_t *const scope = ctx;
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_context_t *const usr = chk->usr;
  MDBX_env *const env = usr->env;
  MDBX_txn *const txn = usr->txn;

  MDBX_chk_table_t *tbl;
  int err = chk_get_tbl(scope, tbl_info, &tbl);
  if (unlikely(err))
    return err;

  if (deep > 42) {
    chk_scope_issue(scope, "too deeply %u, page %zu, parent %zu", deep, pgno, parent_pgno);
    return MDBX_CORRUPTED /* avoid infinite loop/recursion */;
  }
  if (pagetype != page_large)
    histogram_acc(deep, &tbl->histogram.height);
  usr->result.processed_pages += npages;
  const size_t page_bytes = payload_bytes + header_bytes + unused_bytes;

  if (pagetype < page_sub_leaf) {
    const txnid_t basis = txn_basis_snapshot(txn);
    const txnid_t age = (page_txnid >= MIN_TXNID)
                            ? ((page_txnid > basis) ? /* dirty */ 0 : (basis - page_txnid) / xMDBX_TXNID_STEP + 1)
                            : SIZE_MAX;
    histogram_acc_ex((age < SIZE_MAX) ? (size_t)age : SIZE_MAX - 1, &usr->result.histogram_page_age, HISTOGRAM_LE0);
    histogram_acc_ex((age < SIZE_MAX) ? (size_t)age : SIZE_MAX - 1, &tbl->histogram.page_age, HISTOGRAM_LE0);

    histogram_acc(pgno, &tbl->histogram.pgno);
    if (tbl->id != FREE_DBI)
      histogram_acc(pgno, &usr->result.histogram_pgno_payload);
  }

  int height = deep + 1;
  if (tbl->id >= CORE_DBS)
    height -= usr->txn->dbs[MAIN_DBI].height;
  const tree_t *nested = tbl_info->nested;
  if (nested) {
    if (tbl->flags & MDBX_DUPSORT)
      height -= tbl_info->internal->height;
    else {
      chk_object_issue(scope, "nested tree", pgno, "unexpected", "table %s flags 0x%x, deep %i",
                       chk_v2a(chk, &tbl->name), tbl->flags, deep);
      nested = nullptr;
    }
  }

  const char *pagetype_caption;
  bool branch = false;
  struct MDBX_chk_histogram *density = nullptr;
  switch (pagetype) {
  default:
    chk_object_issue(scope, "page", pgno, "unknown page-type", "type %u, deep %i, parent %zu", (unsigned)pagetype, deep,
                     parent_pgno);
    pagetype_caption = "unknown";
    tbl->pages.broken += npages;
    break;
  case page_broken:
    ASSERT(page_err != MDBX_SUCCESS);
    pagetype_caption = "broken";
    tbl->pages.broken += npages;
    break;
  case page_sub_broken:
    ASSERT(page_err != MDBX_SUCCESS);
    pagetype_caption = "broken-subpage";
    tbl->pages.broken += npages;
    break;
  case page_large:
    pagetype_caption = "large";
    histogram_acc(npages, &tbl->histogram.large_pages);
    density = &tbl->histogram.large_or_nested_density;
    if (tbl->flags & MDBX_DUPSORT)
      chk_object_issue(scope, "page", pgno, "unexpected", "type %u, table %s flags 0x%x, deep %i, parent %zu",
                       (unsigned)pagetype, chk_v2a(chk, &tbl->name), tbl->flags, deep, parent_pgno);
    break;
  case page_branch:
    branch = true;
    if (!nested) {
      pagetype_caption = "branch";
      tbl->pages.branch += 1;
      density = &tbl->histogram.tree_density;
    } else {
      pagetype_caption = "nested-branch";
      tbl->pages.nested_branch += 1;
      density = &tbl->histogram.large_or_nested_density;
    }
    break;
  case page_dupfix_leaf:
    if (!nested)
      chk_object_issue(scope, "page", pgno, "unexpected", "type %u, table %s flags 0x%x, deep %i, parent %zu",
                       (unsigned)pagetype, chk_v2a(chk, &tbl->name), tbl->flags, deep, parent_pgno);
    /* fall through */
    __fallthrough;
  case page_leaf:
    if (!nested) {
      pagetype_caption = "leaf";
      tbl->pages.leaf += 1;
      density = &tbl->histogram.tree_density;
      if (height != tbl_info->internal->height)
        chk_object_issue(scope, "page", pgno, "wrong tree height", "actual %i != %i table %s, parent %zu", height,
                         tbl_info->internal->height, chk_v2a(chk, &tbl->name), parent_pgno);
    } else {
      pagetype_caption = (pagetype == page_leaf) ? "nested-leaf" : "nested-leaf-dupfix";
      tbl->pages.nested_leaf += 1;
      density = &tbl->histogram.large_or_nested_density;
      if (chk->last_nested_root != nested->root) {
        histogram_acc(height, &tbl->histogram.nested_height_or_gc_span_length);
        chk->last_nested_root = nested->root;
      }
      if (height != nested->height)
        chk_object_issue(scope, "page", pgno, "wrong nested-tree height", "actual %i != %i dupsort-node %s, parent %zu",
                         height, nested->height, chk_v2a(chk, &tbl->name), parent_pgno);
    }
    break;
  case page_sub_dupfix_leaf:
  case page_sub_leaf:
    pagetype_caption = (pagetype == page_sub_leaf) ? "subleaf-dupsort" : "subleaf-dupfix";
    tbl->pages.nested_subleaf += 1;
    if ((tbl->flags & MDBX_DUPSORT) == 0 || nested)
      chk_object_issue(scope, "page", pgno, "unexpected", "type %u, table %s flags 0x%x, deep %i, parent %zu",
                       (unsigned)pagetype, chk_v2a(chk, &tbl->name), tbl->flags, deep, parent_pgno);
    else
      density = &tbl->histogram.large_or_nested_density;
    break;
  }

  if (density)
    histogram_acc((page_size - unused_bytes) * 100 / page_size, density);

  if (npages) {
    if (tbl->cookie) {
      MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_extra);
      if (npages == 1)
        chk_print(line, "%s-page %" PRIuSIZE, pagetype_caption, pgno);
      else
        chk_print(line, "%s-span %" PRIuSIZE "[%u]", pagetype_caption, pgno, npages);
      chk_line_end(chk_print(
          line, " of %s: header %" PRIiPTR ", %s %" PRIiPTR ", payload %" PRIiPTR ", unused %" PRIiPTR ", deep %i",
          chk_v2a(chk, &tbl->name), header_bytes, (pagetype == page_branch) ? "keys" : "entries", nentries,
          payload_bytes, unused_bytes, deep));
    }

    bool already_used = false;
    for (unsigned n = 0; n < npages; ++n) {
      const size_t spanpgno = pgno + n;
      if (spanpgno >= usr->result.alloc_pages) {
        chk_object_issue(scope, "page", spanpgno, "wrong page-no", "%s-page: %" PRIuSIZE " > %" PRIuSIZE ", deep %i",
                         pagetype_caption, spanpgno, usr->result.alloc_pages, deep);
        tbl->pages.all += 1;
      } else if (chk->pagemap[spanpgno]) {
        const MDBX_chk_table_t *const rival = chk->table[chk->pagemap[spanpgno] - 1];
        chk_object_issue(scope, "page", spanpgno, (branch && rival == tbl) ? "loop" : "already used",
                         "%s-page: by %s, deep %i, parent %zu", pagetype_caption, chk_v2a(chk, &rival->name), deep,
                         parent_pgno);
        already_used = true;
      } else {
        chk->pagemap[spanpgno] = (int16_t)tbl->id + 1;
        tbl->pages.all += 1;
      }
    }

    if (already_used)
      return branch ? MDBX_RESULT_TRUE /* avoid infinite loop/recursion */
                    : MDBX_SUCCESS;
  }

  if (MDBX_IS_ERROR(page_err)) {
    chk_object_issue(scope, "page", pgno, "invalid/corrupted", "%s-page, parent %zu", pagetype_caption, parent_pgno);
  } else {
    if (unused_bytes > page_size)
      chk_object_issue(scope, "page", pgno, "illegal unused-bytes", "%s-page: %u < %" PRIuSIZE " < %u, parent %zu",
                       pagetype_caption, 0, unused_bytes, env->ps, parent_pgno);

    if (header_bytes < (int)sizeof(long) || (size_t)header_bytes >= env->ps - sizeof(long)) {
      chk_object_issue(scope, "page", pgno, "illegal header-length",
                       "%s-page: %" PRIuSIZE " < %" PRIuSIZE " < %" PRIuSIZE ", parent %zu", pagetype_caption,
                       sizeof(long), header_bytes, env->ps - sizeof(long), parent_pgno);
    }
    if (nentries < 1 || (pagetype == page_branch && nentries < 2)) {
      chk_object_issue(scope, "page", pgno, nentries ? "half-empty" : "empty",
                       "%s-page: payload %" PRIuSIZE " bytes, %" PRIuSIZE " entries, deep %i, parent %zu",
                       pagetype_caption, payload_bytes, nentries, deep, parent_pgno);
      tbl->pages.empty += 1;
    }

    if (npages) {
      if (page_bytes != page_size) {
        chk_object_issue(scope, "page", pgno, "misused",
                         "%s-page: %" PRIuPTR " != %" PRIuPTR " (%" PRIuPTR "h + %" PRIuPTR "p + %" PRIuPTR
                         "u), deep %i, parent %zu",
                         pagetype_caption, page_size, page_bytes, header_bytes, payload_bytes, unused_bytes, deep,
                         parent_pgno);
        if (page_size > page_bytes)
          tbl->lost_bytes += page_size - page_bytes;
      } else {
        tbl->payload_bytes += payload_bytes + header_bytes;
        usr->result.total_payload_bytes += payload_bytes + header_bytes;
      }
    }
  }
  return chk_check_break(scope);
}

__cold int chk_tree(MDBX_chk_scope_t *const scope) {
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_context_t *const usr = chk->usr;
  MDBX_env *const env = usr->env;
  MDBX_txn *const txn = usr->txn;

#if IS_WINDOWS
  SetLastError(ERROR_SUCCESS);
#else
  errno = 0;
#endif /* Windows */
  chk->pagemap = osal_calloc(usr->result.alloc_pages, sizeof(*chk->pagemap));
  if (!chk->pagemap) {
    int err = osal_get_errno();
    return chk_error_rc(scope, err ? err : MDBX_ENOMEM, "calloc");
  }

  if (scope->verbosity > MDBX_chk_info)
    chk_scope_push(scope, 0, "Walking pages...");
  /* always skip key ordering checking
   * to avoid MDBX_CORRUPTED in case custom comparators were used */
  usr->result.processed_pages = NUM_METAS;
  /* Keep the pre-#82 `chk` behavior: it has its own loop/dup detection and
   * must inspect reconverging or cyclic trees whole (a future solver will try
   * to salvage the conflicting branch) instead of failing at the first
   * revisit, so the walk revisit guard is disabled here. */
  int err = walk_pages(txn, chk_pgvisitor, scope, dont_check_keys_ordering | dont_check_revisit);
  if (err != MDBX_SUCCESS) {
    chk_error_rc(scope, err, "walk_pages");
    return err;
  }

  for (size_t n = NUM_METAS; n < usr->result.alloc_pages; ++n)
    if (!chk->pagemap[n])
      usr->result.unused_pages += 1;

  MDBX_chk_table_t total;
  memset(&total, 0, sizeof(total));
  total.pages.all = NUM_METAS;
  for (size_t i = 0; i < ARRAY_LENGTH(chk->table) && chk->table[i]; ++i) {
    MDBX_chk_table_t *const tbl = chk->table[i];
    total.payload_bytes += tbl->payload_bytes;
    total.lost_bytes += tbl->lost_bytes;
    total.pages.all += tbl->pages.all;
    total.pages.empty += tbl->pages.empty;
    total.pages.broken += tbl->pages.broken;
    total.pages.branch += tbl->pages.branch;
    total.pages.leaf += tbl->pages.leaf;
    total.pages.nested_branch += tbl->pages.nested_branch;
    total.pages.nested_leaf += tbl->pages.nested_leaf;
    total.pages.nested_subleaf += tbl->pages.nested_subleaf;
  }
  ASSERT(total.pages.all == usr->result.processed_pages);

  const size_t total_page_bytes = pgno2bytes(env, total.pages.all);
  if (usr->scope->subtotal_issues || usr->scope->verbosity >= MDBX_chk_verbose)
    chk_line_end(chk_print(chk_line_begin(usr->scope, MDBX_chk_resolution),
                           "walked %zu pages, left/unused %zu"
                           ", %" PRIuSIZE " problem(s)",
                           usr->result.processed_pages, usr->result.unused_pages, usr->scope->subtotal_issues));

  err = chk_scope_restore(scope, err);
  if (scope->verbosity > MDBX_chk_info) {
    for (size_t i = 0; i < ARRAY_LENGTH(chk->table) && chk->table[i]; ++i) {
      MDBX_chk_table_t *const tbl = chk->table[i];
      MDBX_chk_scope_t *inner =
          chk_scope_push(scope, 0, (tbl->pages.all ? "b-tree %s, subtotal %" PRIuSIZE " pages:" : "b-tree %s: empty"),
                         chk_v2a(chk, &tbl->name), tbl->pages.all);
      if (tbl->pages.all) {
        MDBX_chk_line_t *line = chk_line_begin(inner, MDBX_chk_info);
        if (line) {
          line = chk_puts(line, "pages composition: ");
          if (tbl->pages.broken)
            line = chk_print(line, "broken %" PRIuSIZE ", ", tbl->pages.broken);
          if (tbl->pages.broken != tbl->pages.all) {
            line = chk_print(line, "branch %" PRIuSIZE ", leaf %" PRIuSIZE, tbl->pages.branch, tbl->pages.leaf);
            if (tbl->pages.nested_subleaf || (tbl->flags & MDBX_DUPSORT) != 0)
              line = chk_print(line, ", subleaf %" PRIuSIZE, tbl->pages.nested_subleaf);
            if (tbl->pages.nested_branch || (tbl->flags & MDBX_DUPSORT) != 0)
              line = chk_print(line, ", nested-branch %" PRIuSIZE, tbl->pages.nested_branch);
            if (tbl->pages.nested_leaf || (tbl->flags & MDBX_DUPSORT) != 0)
              line = chk_print(line, ", nested-leaf %" PRIuSIZE, tbl->pages.nested_leaf);
            if (tbl->histogram.large_pages.count || (tbl->flags & MDBX_DUPSORT) == 0) {
              line = chk_print(line, ", large %" PRIuSIZE, tbl->histogram.large_pages.count);
              if (tbl->histogram.large_pages.amount | tbl->histogram.large_pages.count)
                line = histogram_print(inner, line, &tbl->histogram.large_pages, " amount", "single", true);
            }
            line = histogram_dist(chk_line_feed(line), &tbl->histogram.height, "tree levels", "1", false);
            if ((tbl->flags & MDBX_DUPSORT) != 0 ||
                (tbl->histogram.nested_height_or_gc_span_length.count && tbl != &chk->table_gc)) {
              line = chk_print(chk_line_feed(line),
                               "nested tree(s): quantity %" PRIuSIZE ", subtotal pages %" PRIuSIZE ", ",
                               tbl->histogram.nested_height_or_gc_span_length.count,
                               tbl->pages.nested_branch + tbl->pages.nested_leaf);
              if (tbl != &chk->table_gc)
                line = histogram_dist(line, &tbl->histogram.nested_height_or_gc_span_length, "levels", "1", false);
            }
            line = chk_line_feed(line);

            const size_t bytes = pgno2bytes(env, tbl->pages.all);
            line = chk_print_bytes_percent(line, "pages density: subtotal\0\0", bytes, total_page_bytes);
            line = chk_print_percent(line, ", payload\0\0", tbl->payload_bytes, bytes, "");
            line = chk_print_percent(line, ", unused\0\0", bytes - tbl->payload_bytes, bytes, "");
            if (tbl->pages.empty)
              line = chk_print(line, ", %" PRIuSIZE " empty pages", tbl->pages.empty);
            if (tbl->lost_bytes)
              line = chk_print(line, ", %" PRIuSIZE " bytes lost", tbl->lost_bytes);

            line = histogram_dist(chk_line_feed(line), &tbl->histogram.tree_density, "pages %-density distribution",
                                  "1", false);
            line = histogram_dist(chk_line_feed(line), &tbl->histogram.large_or_nested_density,
                                  (tbl->flags & MDBX_DUPSORT) ? "nested %-density distribution"
                                                              : "large pages %-density distribution",
                                  "1", false);
          }
          histogram_dist(chk_line_feed(line), &tbl->histogram.page_age, "pages age distribution", "dirty", false);
          chk_line_end(line);
        }
        if (tbl->histogram.pgno.count) {
          line = chk_line_begin(inner, (tbl->id != FREE_DBI) ? MDBX_chk_verbose : MDBX_chk_info);
          histogram_dist(line, &tbl->histogram.pgno, "pgno distribution", "bad", false);
          chk_line_end(line);
        }
      }
      chk_scope_restore(scope, 0);
    }
  }

  MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_resolution);
  line = chk_print(line, "summary: total %" PRIuSIZE " bytes", total_page_bytes);
  line =
      chk_print_percent(line, ", payload\0 average density\0", usr->result.total_payload_bytes, total_page_bytes, "");
  line = chk_print_percent(line, ", unused\0 average sparsity\0", total_page_bytes - usr->result.total_payload_bytes,
                           total_page_bytes, "");
  if (total.pages.empty)
    line = chk_print(line, ", %" PRIuSIZE " empty pages", total.pages.empty);
  if (total.lost_bytes)
    line = chk_print(line, ", %" PRIuSIZE " bytes lost", total.lost_bytes);
  chk_line_end(line);
  return err;
}

typedef int(chk_kv_visitor)(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                            const MDBX_val *key, const MDBX_val *data);

__cold int chk_handle_kv(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                                const MDBX_val *key, const MDBX_val *data) {
  MDBX_chk_internal_t *const chk = scope->internal;
  int err = MDBX_SUCCESS;
  /* cookie == nullptr means the table was filtered out by the user (see the
   * table_filter callback, e.g. `mdbx_chk -s <table>`); no per-record user
   * processing for such tables. In particular the MAIN table always carries a
   * cookie only when it passes the filter, so this is not an invariant to
   * assert on (issue #49: ASSERT(tbl->cookie) crashed mdbx_chk -s on DBs
   * whose main table holds ordinary records). */
  if (!tbl->cookie)
    return err;
  if (chk->cb->table_handle_kv)
    err = chk->cb->table_handle_kv(chk->usr, tbl, record_number, key, data);
  return err ? err : chk_check_break(scope);
}

__cold int chk_db(MDBX_chk_scope_t *const scope, MDBX_dbi dbi, MDBX_chk_table_t *tbl, chk_kv_visitor *handler) {
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_context_t *const usr = chk->usr;
  MDBX_env *const env = usr->env;
  MDBX_txn *const txn = usr->txn;
  MDBX_cursor *cursor = nullptr;
  size_t record_count = 0, dups = 0, sub_databases = 0;
  int err;

  if ((MDBX_TXN_FINISHED | MDBX_TXN_ERROR) & txn->flags) {
    chk_line_end(chk_flush(chk_print(chk_line_begin(scope, MDBX_chk_error),
                                     "abort processing %s due to a previous error", chk_v2a(chk, &tbl->name))));
    err = MDBX_BAD_TXN;
    goto bailout;
  }

  if (0 > (int)dbi) {
    /* Opening a named table with custom comparators is not possible via
     * MDBX_DB_ACCEDE: the engine refuses to bind different comparators to an
     * already-bound/valid table (MDBX_INCOMPATIBLE), which made `mdbx_chk -i`
     * fail/crash on any database containing named sub-tables. Order-tolerance
     * for MDBX_CHK_IGNORE_ORDER is provided by the z_ignord cursor flag and by
     * the IGNORE_ORDER guards around the order-error reporting below, so no
     * custom comparators are needed here. */
    err = dbi_open(txn, &tbl->name, MDBX_DB_ACCEDE, &dbi, nullptr, nullptr);
    if (unlikely(err)) {
      chk_error_rc(scope, err, "mdbx_dbi_open");
      goto bailout;
    }
    tASSERT0(txn, dbi < txn->env->n_dbi && (txn->env->dbs_flags[dbi] & DB_VALID) != 0);
  }

  const tree_t *const db = txn->dbs + dbi;
  if (handler) {
    const char *key_mode = nullptr;
    switch (tbl->flags & (MDBX_REVERSEKEY | MDBX_INTEGERKEY)) {
    case 0:
      key_mode = "usual";
      break;
    case MDBX_REVERSEKEY:
      key_mode = "reverse";
      break;
    case MDBX_INTEGERKEY:
      key_mode = "ordinal";
      break;
    case MDBX_REVERSEKEY | MDBX_INTEGERKEY:
      key_mode = "msgpack";
      break;
    default:
      key_mode = "inconsistent";
      chk_scope_issue(scope, "wrong key-mode (0x%x)", tbl->flags & (MDBX_REVERSEKEY | MDBX_INTEGERKEY));
    }

    const char *value_mode = nullptr;
    switch (tbl->flags & (MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_DUPFIXED | MDBX_INTEGERDUP)) {
    case 0:
      value_mode = "single";
      break;
    case MDBX_DUPSORT:
      value_mode = "multi";
      break;
    case MDBX_DUPSORT | MDBX_REVERSEDUP:
      value_mode = "multi-reverse";
      break;
    case MDBX_DUPSORT | MDBX_DUPFIXED:
      value_mode = "multi-samelength";
      break;
    case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP:
      value_mode = "multi-reverse-samelength";
      break;
    case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP:
      value_mode = "multi-ordinal";
      break;
    case MDBX_DUPSORT | MDBX_INTEGERDUP | MDBX_REVERSEDUP:
      value_mode = "multi-msgpack";
      break;
    case MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_REVERSEDUP:
      value_mode = "reserved";
      break;
    default:
      value_mode = "inconsistent";
      chk_scope_issue(scope, "wrong value-mode (0x%x)",
                      tbl->flags & (MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_DUPFIXED | MDBX_INTEGERDUP));
    }

    MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_info);
    line = chk_print(line, "key-value kind: %s-key => %s-value", key_mode, value_mode);
    line = chk_print(line, ", flags:");
    if (!tbl->flags)
      line = chk_print(line, " none");
    else {
      const uint8_t f[] = {
          MDBX_DUPSORT, MDBX_INTEGERKEY, MDBX_REVERSEKEY, MDBX_DUPFIXED, MDBX_REVERSEDUP, MDBX_INTEGERDUP, 0};
      const char *const t[] = {"dupsort", "integerkey", "reversekey", "dupfix", "reversedup", "integerdup"};
      for (size_t i = 0; f[i]; i++)
        if (tbl->flags & f[i])
          line = chk_print(line, " %s", t[i]);
    }
    chk_line_end(chk_print(line, " (0x%02X)", tbl->flags));

    line = chk_print(chk_line_begin(scope, MDBX_chk_verbose), "entries %" PRIu64 ", sequence %" PRIu64, db->items,
                     db->sequence);
    if (db->mod_txnid)
      line = chk_print(line, ", last modification txn#%" PRIaTXN, db->mod_txnid);
    if (db->root != P_INVALID)
      line = chk_print(line, ", root #%" PRIaPGNO, db->root);
    chk_line_end(line);
    chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_verbose),
                           "b-tree depth %u, pages: branch %" PRIaPGNO ", leaf %" PRIaPGNO ", large %" PRIaPGNO,
                           db->height, db->branch_pages, db->leaf_pages, db->large_pages));

    if ((chk->flags & MDBX_CHK_SKIP_BTREE_TRAVERSAL) == 0) {
      const size_t branch_pages = tbl->pages.branch + tbl->pages.nested_branch;
      const size_t leaf_pages = tbl->pages.leaf + tbl->pages.nested_leaf;
      const size_t subtotal_pages = db->branch_pages + db->leaf_pages + db->large_pages;
      if (subtotal_pages != tbl->pages.all)
        chk_scope_issue(scope, "%s pages mismatch (%" PRIuSIZE " != walked %" PRIuSIZE ")", "subtotal", subtotal_pages,
                        tbl->pages.all);
      if (db->branch_pages != branch_pages)
        chk_scope_issue(scope, "%s pages mismatch (%" PRIaPGNO " != walked %" PRIuSIZE ")", "branch", db->branch_pages,
                        branch_pages);
      if (db->leaf_pages != leaf_pages)
        chk_scope_issue(scope, "%s pages mismatch (%" PRIaPGNO " != walked %" PRIuSIZE ")", "all-leaf", db->leaf_pages,
                        leaf_pages);
      if (db->large_pages != tbl->histogram.large_pages.amount)
        chk_scope_issue(scope, "%s pages mismatch (%" PRIaPGNO " != walked %" PRIuSIZE ")", "large/overflow",
                        db->large_pages, tbl->histogram.large_pages.amount);
    }
  }

  err = mdbx_cursor_open(txn, dbi, &cursor);
  if (unlikely(err)) {
    chk_error_rc(scope, err, "mdbx_cursor_open");
    goto bailout;
  }
  if (chk->flags & MDBX_CHK_IGNORE_ORDER) {
    cursor->checking |= z_ignord | z_pagecheck;
    if (cursor->subcur)
      cursor->subcur->cursor.checking |= z_ignord | z_pagecheck;
  }

  const size_t maxkeysize = mdbx_env_get_maxkeysize_ex(env, tbl->flags);
  MDBX_val prev_key = {nullptr, 0}, prev_data = {nullptr, 0};
  MDBX_val key, data;
  size_t dups_count = 0;
  err = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
  while (err == MDBX_SUCCESS) {
    err = chk_check_break(scope);
    if (unlikely(err))
      goto bailout;

    bool bad_key = false;
    if (key.iov_len > maxkeysize) {
      chk_object_issue(scope, "entry", record_count, "key length exceeds max-key-size", "%" PRIuPTR " > %" PRIuPTR,
                       key.iov_len, maxkeysize);
      bad_key = true;
    } else if ((tbl->flags & MDBX_INTEGERKEY) && key.iov_len != 8 && key.iov_len != 4) {
      chk_object_issue(scope, "entry", record_count, "wrong key length", "%" PRIuPTR " != 4or8", key.iov_len);
      bad_key = true;
    }

    bool bad_data = false;
    if ((tbl->flags & MDBX_INTEGERDUP) && data.iov_len != 8 && data.iov_len != 4) {
      chk_object_issue(scope, "entry", record_count, "wrong data length", "%" PRIuPTR " != 4or8", data.iov_len);
      bad_data = true;
    }

    if (prev_key.iov_base) {
      if (key.iov_base == prev_key.iov_base)
        dups_count += 1;
      else {
        histogram_acc(dups_count, &tbl->histogram.multival);
        dups_count = 0;
      }
      if (prev_data.iov_base && !bad_data && (tbl->flags & MDBX_DUPFIXED) && prev_data.iov_len != data.iov_len) {
        chk_object_issue(scope, "entry", record_count, "different data length", "%" PRIuPTR " != %" PRIuPTR,
                         prev_data.iov_len, data.iov_len);
        bad_data = true;
      }

      if (!bad_key) {
        int cmp = mdbx_cmp(txn, dbi, &key, &prev_key);
        if (cmp == 0) {
          ++dups;
          if ((tbl->flags & MDBX_DUPSORT) == 0) {
            chk_object_issue(scope, "entry", record_count, "duplicated entries", nullptr);
            if (prev_data.iov_base && data.iov_len == prev_data.iov_len &&
                memcmp(data.iov_base, prev_data.iov_base, data.iov_len) == 0)
              chk_object_issue(scope, "entry", record_count, "complete duplicate", nullptr);
          } else if (!bad_data && prev_data.iov_base) {
            cmp = mdbx_dcmp(txn, dbi, &data, &prev_data);
            if (cmp == 0)
              chk_object_issue(scope, "entry", record_count, "complete duplicate", nullptr);
            else if (cmp < 0 && !(chk->flags & MDBX_CHK_IGNORE_ORDER))
              chk_object_issue(scope, "entry", record_count, "wrong order of multi-values", nullptr);
          }
        } else if (cmp < 0 && !(chk->flags & MDBX_CHK_IGNORE_ORDER))
          chk_object_issue(scope, "entry", record_count, "wrong order of entries", nullptr);
      }
    }

    if (!bad_key) {
      if (!prev_key.iov_base && (tbl->flags & MDBX_INTEGERKEY))
        chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_info), "fixed key-size %" PRIuSIZE, key.iov_len));
      prev_key = key;
    }
    if (!bad_data) {
      if (!prev_data.iov_base && (tbl->flags & (MDBX_INTEGERDUP | MDBX_DUPFIXED)))
        chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_info), "fixed data-size %" PRIuSIZE, data.iov_len));
      prev_data = data;
    }

    record_count++;
    histogram_acc(key.iov_len, &tbl->histogram.key_len);
    histogram_acc(data.iov_len, &tbl->histogram.val_len);

    const node_t *const node = page_node(cursor->pg[cursor->top], cursor->ki[cursor->top]);
    if (node_flags(node) == N_TREE) {
      if (dbi != MAIN_DBI || (tbl->flags & (MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP | MDBX_INTEGERDUP)))
        chk_object_issue(scope, "entry", record_count, "unexpected table", "node-flags 0x%x", node_flags(node));
      else if (data.iov_len != sizeof(tree_t))
        chk_object_issue(scope, "entry", record_count, "wrong table node size", "node-size %" PRIuSIZE " != %" PRIuSIZE,
                         data.iov_len, sizeof(tree_t));
      else if (scope->stage == MDBX_chk_maindb)
        /* подсчитываем table при первом проходе */
        sub_databases += 1;
      else {
        /* обработка table при втором проходе */
        tree_t aligned_db;
        memcpy(&aligned_db, data.iov_base, sizeof(aligned_db));
        walk_tbl_t tbl_info = {.name = key};
        tbl_info.internal = &aligned_db;
        MDBX_chk_table_t *table;
        err = chk_get_tbl(scope, &tbl_info, &table);
        if (unlikely(err))
          goto bailout;
        if (table->cookie) {
          err = chk_scope_begin(chk, 0, MDBX_chk_tables, table, &usr->result.problems_kv, "Processing table %s...",
                                chk_v2a(chk, &table->name));
          if (likely(!err)) {
            err = chk_db(usr->scope, (MDBX_dbi)-1, table, chk_handle_kv);
            if (err != MDBX_EINTR && err != MDBX_RESULT_TRUE)
              usr->result.table_processed += 1;
          }
          err = chk_scope_restore(scope, err);
          if (unlikely(err))
            goto bailout;
        } else
          chk_line_end(chk_flush(chk_print(chk_line_begin(scope, MDBX_chk_processing), "Skip processing %s...",
                                           chk_v2a(chk, &table->name))));
      }
    } else if (handler) {
      err = handler(scope, tbl, record_count, &key, &data);
      if (unlikely(err))
        goto bailout;
    }

    err = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
  }

  if (prev_key.iov_base)
    histogram_acc(dups_count, &tbl->histogram.multival);

  err = (err != MDBX_NOTFOUND) ? chk_error_rc(scope, err, "mdbx_cursor_get") : MDBX_SUCCESS;
  if (err == MDBX_SUCCESS && record_count != db->items)
    chk_scope_issue(scope, "different number of entries %" PRIuSIZE " != %" PRIu64, record_count, db->items);
bailout:
  if (cursor) {
    if (handler) {
      if (record_count) {
        MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_info);
        line = histogram_dist(line, &tbl->histogram.key_len, "key length distribution", "0/1", false);
        chk_line_feed(line);
        line = histogram_dist(line, &tbl->histogram.val_len, "value length distribution", "0/1", false);
        if (tbl->histogram.multival.amount) {
          chk_line_feed(line);
          line = histogram_dist(line, &tbl->histogram.multival, "number of multi-values distribution", "single", false);
          chk_line_feed(line);
          line =
              chk_print(line, "number of keys %" PRIuSIZE ", average values per key ", tbl->histogram.multival.count);
          line = chk_print_ratio(line, record_count, tbl->histogram.multival.count, 1);
        }
        chk_line_end(line);
      }
      if (scope->stage == MDBX_chk_maindb)
        usr->result.table_total = sub_databases;
      if (chk->cb->table_conclude)
        err = chk->cb->table_conclude(usr, tbl, cursor, err);
      MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_resolution);
      if (record_count | tbl->histogram.key_len.amount | tbl->histogram.val_len.amount | scope->subtotal_issues) {
        line = chk_print(line, "summary: %" PRIuSIZE " records,", record_count);
        if (dups || (tbl->flags & (MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP | MDBX_INTEGERDUP)))
          line = chk_print(line, " %" PRIuSIZE " dups,", dups);
        if (sub_databases || dbi == MAIN_DBI)
          line = chk_print(line, " %" PRIuSIZE " tables,", sub_databases);
        line = chk_print(line,
                         " %" PRIuSIZE " key's bytes,"
                         " %" PRIuSIZE " data's bytes,"
                         " %" PRIuSIZE " problem(s)",
                         tbl->histogram.key_len.amount, tbl->histogram.val_len.amount, scope->subtotal_issues);
      } else {
        line = chk_puts(line, "empty");
      }
      chk_line_end(chk_flush(line));
    }

    mdbx_cursor_close(cursor);
  }
  if (!txn->cursors[dbi] && (txn->dbi_state[dbi] & DBI_FRESH))
    mdbx_dbi_close(env, dbi);

  return err;
}

__cold int chk_handle_gc(MDBX_chk_scope_t *const scope, MDBX_chk_table_t *tbl, const size_t record_number,
                                const MDBX_val *key, const MDBX_val *data) {
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_context_t *const usr = chk->usr;
  ASSERT(tbl == &chk->table_gc);
  (void)tbl;
  const char *bad = "";

  if (key->iov_len != sizeof(txnid_t))
    chk_object_issue(scope, "entry", record_number, "wrong txn-id size", "key-size %" PRIuSIZE, key->iov_len);
  else {
    const txnid_t txnid = unaligned_peek_u64(4, key->iov_base);
    if (txnid < 1 || txnid > usr->txn->txnid)
      chk_object_issue(scope, "entry", record_number, "wrong txn-id", "%" PRIaTXN, txnid);
    else {
      const const_pnl_t pnl = data->iov_base;
      if (data->iov_len < sizeof(pgno_t) || data->iov_len % sizeof(pgno_t))
        chk_object_issue(scope, "entry", txnid, "wrong pnl size", "%" PRIuPTR, data->iov_len);
      if ((size_t)data->iov_base % sizeof(pgno_t))
        chk_object_issue(scope, "entry", txnid, "invalid pnl alignment", "%" PRIuPTR, (size_t)data->iov_base);
      size_t number = (data->iov_len >= sizeof(pgno_t) && (size_t)data->iov_base % sizeof(pgno_t) == 0) ? *pnl : 0;
      bool clamped = false;
      if (number > PAGELIST_LIMIT) {
        chk_object_issue(scope, "entry", txnid, "wrong idl length", "%" PRIuPTR, number);
        /* clamp the count to what the record can actually hold, so that the
         * accounting and the per-entry walk below cannot read past it */
        number = (data->iov_len >= sizeof(pgno_t)) ? data->iov_len / sizeof(pgno_t) - 1 : 0;
        clamped = true;
      } else if ((number + 1) * sizeof(pgno_t) > data->iov_len) {
        chk_object_issue(scope, "entry", txnid, "trimmed idl", "%" PRIuSIZE " > %" PRIuSIZE " (corruption)",
                         (number + 1) * sizeof(pgno_t), data->iov_len);
        number = data->iov_len / sizeof(pgno_t) - 1;
        clamped = true;
      } else if (data->iov_len - (number + 1) * sizeof(pgno_t) >=
                 /* LY: allow gap up to two page. it is ok
                  * and better than shrink-and-retry inside gc_update() */
                 usr->env->ps * 2)
        chk_object_issue(scope, "entry", txnid, "extra idl space",
                         "%" PRIuSIZE " < %" PRIuSIZE " (minor, not a trouble)", (number + 1) * sizeof(pgno_t),
                         data->iov_len);

      usr->result.gc_pages += number;
      if (chk->envinfo.mi_latter_reader_txnid >= txnid)
        usr->result.reclaimable_pages += number;

      size_t prev = MDBX_PNL_ASCENDING ? NUM_METAS - 1 : usr->txn->geo.first_unallocated;
      for (size_t i = 1; i <= number; ++i) {
        const size_t pgno = pnl[i];
        histogram_acc(pgno, (chk->envinfo.mi_latter_reader_txnid > txnid)
                                ? /* account reclaimable as GC-related*/ &tbl->histogram.pgno
                                : &usr->result.histogram_pgno_retained);

        if (pgno < NUM_METAS)
          chk_object_issue(scope, "entry", txnid, "wrong idl entry", "pgno %" PRIuSIZE " < meta-pages %u", pgno,
                           NUM_METAS);
        else if (pgno >= usr->result.backed_pages)
          chk_object_issue(scope, "entry", txnid, "wrong idl entry", "pgno %" PRIuSIZE " > dxb-file-pages %" PRIuSIZE,
                           pgno, usr->result.backed_pages);
        else if (pgno >= usr->result.alloc_pages)
          chk_object_issue(scope, "entry", txnid, "wrong idl entry", "pgno %" PRIuSIZE " > alloc-pages %" PRIuSIZE,
                           pgno, usr->result.alloc_pages - 1);
        else {
          if (unlikely(MDBX_PNL_DISORDERED(prev, pgno))) {
            bad = " [bad sequence]";
            chk_object_issue(scope, "entry", txnid, "bad sequence", "%" PRIuSIZE " %c [%" PRIuSIZE "].%" PRIuSIZE, prev,
                             (prev == pgno) ? '=' : (MDBX_PNL_ASCENDING ? '>' : '<'), i, pgno);
          }
          if (chk->pagemap) {
            const intptr_t id = chk->pagemap[pgno];
            if (id == 0)
              chk->pagemap[pgno] = -1 /* mark the pgno listed in GC */;
            else if (id > 0) {
              ASSERT(id - 1 <= (intptr_t)ARRAY_LENGTH(chk->table));
              chk_object_issue(scope, "page", pgno, "already used", "by %s", chk_v2a(chk, &chk->table[id - 1]->name));
            } else
              chk_object_issue(scope, "page", pgno, "already listed in GC", nullptr);
          }
        }
        prev = pgno;
      }
      if (tbl->cookie && !clamped) {
        /* Skip the span-details pass for a clamped record: its raw leading
         * count is a crafted value, and the pnl_maxspan/pnl_scan_span helpers
         * would read past the record by trusting it. */
        chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_details),
                               "transaction %" PRIaTXN ", %" PRIuSIZE " pages, maxspan %" PRIuSIZE "%s", txnid, number,
                               pnl_maxspan(pnl), bad));
        for (size_t span, i = 1; i <= number; i += span) {
          span = pnl_scan_span(pnl, i);
          const size_t pgno = pnl[i];
          histogram_acc(span, &tbl->histogram.nested_height_or_gc_span_length);
          MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_extra);
          if (line) {
            if (span > 1)
              line = chk_print(line, "%9" PRIuSIZE "[%" PRIuSIZE "]", pgno, span);
            else
              line = chk_print(line, "%9" PRIuSIZE, pgno);
            chk_line_end(line);
            int err = chk_check_break(scope);
            if (err)
              return err;
          }
        }
      }
    }
  }
  return chk_check_break(scope);
}
