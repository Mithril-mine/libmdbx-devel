/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file chk.c — mdbx_env_chk() entry point: setup/teardown, scope stack,
/// value-rendering and orchestration; includes the walk and report parts.

#include "internals.h"

__cold int chk_check_break(MDBX_chk_scope_t *const scope) {
  MDBX_chk_internal_t *const chk = scope->internal;
  return (chk->got_break || (chk->cb->check_break && (chk->got_break = chk->cb->check_break(chk->usr))))
             ? MDBX_RESULT_TRUE
             : MDBX_RESULT_FALSE;
}

//-----------------------------------------------------------------------------

__cold int chk_error_rc(MDBX_chk_scope_t *const scope, int err, const char *subj) {
  MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_error);
  MDBX_chk_internal_t *const chk = scope->internal;
  const char *msg =
      ((err == MDBX_RESULT_TRUE || err == MDBX_EINTR) && chk->cb->check_break && chk->cb->check_break(chk->usr))
          ? "user break"
          : mdbx_strerror(err);
  if (line)
    chk_line_end(chk_flush(chk_print(line, "%s() failed, error %s (%d)", subj, msg, err)));
  else
    debug_log(MDBX_LOG_ERROR, "mdbx_env_chk", 0, "%s() failed, error %s (%d)", subj, msg, err);
  return err;
}

__cold void MDBX_PRINTF_ARGS(5, 6)
    chk_object_issue(MDBX_chk_scope_t *const scope, const char *object, uint64_t entry_number, const char *caption,
                     const char *extra_fmt, ...) {
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_issue_t *issue = chk->usr->scope->issues;
  while (issue) {
    if (issue->caption == caption) {
      issue->count += 1;
      break;
    } else
      issue = issue->next;
  }
  const bool fresh = issue == nullptr;
  if (fresh) {
    issue = osal_malloc(sizeof(*issue));
    if (likely(issue)) {
      issue->caption = caption;
      issue->count = 1;
      issue->next = chk->usr->scope->issues;
      chk->usr->scope->issues = issue;
    } else
      chk_error_rc(scope, MDBX_ENOMEM, "adding issue");
  }

  va_list args;
  va_start(args, extra_fmt);
  if (chk->cb->issue) {
    mdbx_env_chk_encount_problem(chk->usr);
    chk->cb->issue(chk->usr, object, entry_number, caption, extra_fmt, args);
  } else {
    MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_error);
    if (entry_number != UINT64_MAX)
      chk_print(line, "%s #%" PRIu64 ": %s", object, entry_number, caption);
    else
      chk_print(line, "%s: %s", object, caption);
    if (extra_fmt)
      chk_puts(chk_print_va(chk_puts(line, " ("), extra_fmt, args), ")");
    chk_line_end(fresh ? chk_flush(line) : line);
  }
  va_end(args);
}

__cold void MDBX_PRINTF_ARGS(2, 3) chk_scope_issue(MDBX_chk_scope_t *const scope, const char *fmt, ...) {
  MDBX_chk_internal_t *const chk = scope->internal;
  va_list args;
  va_start(args, fmt);
  if (likely(chk->cb->issue)) {
    mdbx_env_chk_encount_problem(chk->usr);
    chk->cb->issue(chk->usr, nullptr, 0, nullptr, fmt, args);
  } else
    chk_line_end(chk_print_va(chk_line_begin(scope, MDBX_chk_error), fmt, args));
  va_end(args);
}

__cold int chk_scope_end(MDBX_chk_internal_t *chk, int err) {
  ASSERT(chk->scope_depth > 0);
  MDBX_chk_scope_t *const inner = chk->scope_stack + chk->scope_depth;
  MDBX_chk_scope_t *const outer = chk->scope_depth ? inner - 1 : nullptr;
  if (!outer || outer->stage != inner->stage) {
    if (err == MDBX_SUCCESS && *chk->problem_counter)
      err = MDBX_PROBLEM;
    else if (*chk->problem_counter == 0 && MDBX_IS_ERROR(err))
      *chk->problem_counter = 1;
    if (chk->problem_counter != &chk->usr->result.total_problems) {
      chk->usr->result.total_problems += *chk->problem_counter;
      chk->problem_counter = &chk->usr->result.total_problems;
    }
    if (chk->cb->stage_end)
      err = chk->cb->stage_end(chk->usr, inner->stage, err);
  }
  if (chk->cb->scope_conclude)
    err = chk->cb->scope_conclude(chk->usr, outer, inner, err);
  chk->usr->scope = outer;
  chk->usr->scope_nesting = chk->scope_depth -= 1;
  if (outer)
    outer->subtotal_issues += inner->subtotal_issues;
  if (chk->cb->scope_pop)
    chk->cb->scope_pop(chk->usr, outer, inner);

  while (inner->issues) {
    MDBX_chk_issue_t *next = inner->issues->next;
    osal_free(inner->issues);
    inner->issues = next;
  }
  memset(inner, -1, sizeof(*inner));
  return err;
}

__cold static int chk_scope_begin_args(MDBX_chk_internal_t *chk, int verbosity_adjustment, enum MDBX_chk_stage stage,
                                       const void *object, size_t *problems, const char *fmt, va_list args) {
  if (unlikely(chk->scope_depth + 1u >= ARRAY_LENGTH(chk->scope_stack)))
    return MDBX_BACKLOG_DEPLETED;

  MDBX_chk_scope_t *const outer = chk->scope_stack + chk->scope_depth;
  const int verbosity = outer->verbosity + (verbosity_adjustment - 1) * (1 << MDBX_chk_severity_prio_shift);
  MDBX_chk_scope_t *const inner = outer + 1;
  memset(inner, 0, sizeof(*inner));
  inner->internal = outer->internal;
  inner->stage = stage ? stage : (stage = outer->stage);
  inner->object = object;
  inner->verbosity = (verbosity < MDBX_chk_warning) ? MDBX_chk_warning : (enum MDBX_chk_severity)verbosity;
  if (problems)
    chk->problem_counter = problems;
  else if (!chk->problem_counter || outer->stage != stage)
    chk->problem_counter = &chk->usr->result.total_problems;

  if (chk->cb->scope_push) {
    const int err = chk->cb->scope_push(chk->usr, outer, inner, fmt, args);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }
  chk->usr->scope = inner;
  chk->usr->scope_nesting = chk->scope_depth += 1;

  if (stage != outer->stage && chk->cb->stage_begin) {
    int err = chk->cb->stage_begin(chk->usr, stage);
    if (unlikely(err != MDBX_SUCCESS)) {
      err = chk_scope_end(chk, err);
      ASSERT(err != MDBX_SUCCESS);
      return err ? err : MDBX_RESULT_TRUE;
    }
  }
  return MDBX_SUCCESS;
}

__cold int MDBX_PRINTF_ARGS(6, 7)
    chk_scope_begin(MDBX_chk_internal_t *chk, int verbosity_adjustment, enum MDBX_chk_stage stage, const void *object,
                    size_t *problems, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int rc = chk_scope_begin_args(chk, verbosity_adjustment, stage, object, problems, fmt, args);
  va_end(args);
  return rc;
}

__cold int chk_scope_restore(MDBX_chk_scope_t *const target, int err) {
  MDBX_chk_internal_t *const chk = target->internal;
  ASSERT(target <= chk->usr->scope);
  while (chk->usr->scope > target)
    err = chk_scope_end(chk, err);
  return err;
}

__cold void chk_scope_pop(MDBX_chk_scope_t *const inner) {
  if (inner && inner > inner->internal->scope_stack)
    chk_scope_restore(inner - 1, MDBX_SUCCESS);
}

__cold MDBX_chk_scope_t *MDBX_PRINTF_ARGS(3, 4)
    chk_scope_push(MDBX_chk_scope_t *const scope, int verbosity_adjustment, const char *fmt, ...) {
  chk_scope_restore(scope, MDBX_SUCCESS);
  va_list args;
  va_start(args, fmt);
  int err = chk_scope_begin_args(scope->internal, verbosity_adjustment, scope->stage, nullptr, nullptr, fmt, args);
  va_end(args);
  return err ? nullptr : scope + 1;
}

__cold const char *chk_v2a(MDBX_chk_internal_t *chk, const MDBX_val *val) {
  if (val == MDBX_CHK_MAIN)
    return "@MAIN";
  if (val == MDBX_CHK_GC)
    return "@GC";
  if (val == MDBX_CHK_META)
    return "@META";

  const unsigned char *const data = val->iov_base;
  const size_t len = val->iov_len;
  if (data == MDBX_CHK_MAIN)
    return "@MAIN";
  if (data == MDBX_CHK_GC)
    return "@GC";
  if (data == MDBX_CHK_META)
    return "@META";

  if (!len)
    return "<zero-length>";
  if (!data)
    return "<nullptr>";
  if (len > 65536) {
    const size_t enough = 42;
    if (chk->v2a_buf.iov_len < enough) {
      void *ptr = osal_realloc(chk->v2a_buf.iov_base, enough);
      if (unlikely(!ptr))
        return "<out-of-memory>";
      chk->v2a_buf.iov_base = ptr;
      chk->v2a_buf.iov_len = enough;
    }
    snprintf(chk->v2a_buf.iov_base, chk->v2a_buf.iov_len, "<too-long.%" PRIuSIZE ">", len);
    return chk->v2a_buf.iov_base;
  }

  bool printable = true;
  bool quoting = false;
  size_t xchars = 0;
  for (size_t i = 0; i < len && printable; ++i) {
    quoting = quoting || !(data[i] == '_' || isalnum(data[i]));
    printable = isprint(data[i]) || (data[i] < ' ' && ++xchars < 4 && len > xchars * 4);
  }

  size_t need = len + 1;
  if (quoting || !printable)
    need += len + /* quotes */ 2 + 2 * /* max xchars */ 4;
  if (need > chk->v2a_buf.iov_len) {
    void *ptr = osal_realloc(chk->v2a_buf.iov_base, need);
    if (unlikely(!ptr))
      return "<out-of-memory>";
    chk->v2a_buf.iov_base = ptr;
    chk->v2a_buf.iov_len = need;
  }

  static const char hex[] = "0123456789abcdef";
  char *w = chk->v2a_buf.iov_base;
  if (!quoting) {
    memcpy(w, data, len);
    w += len;
  } else if (printable) {
    *w++ = '\'';
    for (size_t i = 0; i < len; ++i) {
      if (data[i] < ' ') {
        ASSERT((char *)chk->v2a_buf.iov_base + chk->v2a_buf.iov_len > w + 4);
        w[0] = '\\';
        w[1] = 'x';
        w[2] = hex[data[i] >> 4];
        w[3] = hex[data[i] & 15];
        w += 4;
      } else if (strchr("\"'`\\", data[i])) {
        ASSERT((char *)chk->v2a_buf.iov_base + chk->v2a_buf.iov_len > w + 2);
        w[0] = '\\';
        w[1] = data[i];
        w += 2;
      } else {
        ASSERT((char *)chk->v2a_buf.iov_base + chk->v2a_buf.iov_len > w + 1);
        *w++ = data[i];
      }
    }
    *w++ = '\'';
  } else {
    *w++ = '\\';
    *w++ = 'x';
    for (size_t i = 0; i < len; ++i) {
      ASSERT((char *)chk->v2a_buf.iov_base + chk->v2a_buf.iov_len > w + 2);
      w[0] = hex[data[i] >> 4];
      w[1] = hex[data[i] & 15];
      w += 2;
    }
  }
  ASSERT((char *)chk->v2a_buf.iov_base + chk->v2a_buf.iov_len > w);
  *w = 0;
  return chk->v2a_buf.iov_base;
}

__cold static void chk_dispose(MDBX_chk_internal_t *chk) {
  ASSERT(chk->table[FREE_DBI] == &chk->table_gc);
  ASSERT(chk->table[MAIN_DBI] == &chk->table_main);
  for (size_t i = 0; i < ARRAY_LENGTH(chk->table); ++i) {
    MDBX_chk_table_t *const tbl = chk->table[i];
    if (tbl) {
      chk->table[i] = nullptr;
      if (chk->cb->table_dispose && tbl->cookie) {
        chk->cb->table_dispose(chk->usr, tbl);
        tbl->cookie = nullptr;
      }
      if (tbl != &chk->table_gc && tbl != &chk->table_main)
        osal_free(tbl);
    }
  }
  osal_free(chk->v2a_buf.iov_base);
  osal_free(chk->pagemap);
  chk->usr->internal = nullptr;
  chk->usr->scope = nullptr;
  chk->usr->result.tables = nullptr; /* points into the freed `chk` */
  chk->pagemap = nullptr;
  memset(chk, 0xDD, sizeof(*chk));
  osal_free(chk);
}

//-----------------------------------------------------------------------------

__cold int chk_get_tbl(MDBX_chk_scope_t *const scope, const walk_tbl_t *in, MDBX_chk_table_t **out) {
  MDBX_chk_internal_t *const chk = scope->internal;
  if (chk->last_lookup && chk->last_lookup->name.iov_base == in->name.iov_base) {
    *out = chk->last_lookup;
    return MDBX_SUCCESS;
  }

  for (size_t i = 0; i < ARRAY_LENGTH(chk->table); ++i) {
    MDBX_chk_table_t *tbl = chk->table[i];
    if (!tbl) {
      tbl = osal_calloc(1, sizeof(MDBX_chk_table_t));
      if (unlikely(!tbl)) {
        *out = nullptr;
        return chk_error_rc(scope, MDBX_ENOMEM, "alloc_table");
      }
      chk->table[i] = tbl;
      tbl->flags = in->internal->flags;
      tbl->id = -1;
      tbl->name = in->name;
    }
    if (tbl->name.iov_base == in->name.iov_base) {
      if (tbl->id < 0) {
        tbl->id = (int)i;
        tbl->cookie =
            chk->cb->table_filter ? chk->cb->table_filter(chk->usr, &tbl->name, tbl->flags) : (void *)(intptr_t)-1;
      }
      *out = (chk->last_lookup = tbl);
      return MDBX_SUCCESS;
    }
  }
  chk_scope_issue(scope, "too many tables > %u", (unsigned)ARRAY_LENGTH(chk->table) - CORE_DBS - /* meta */ 1);
  *out = nullptr;
  return MDBX_PROBLEM;
}

//------------------------------------------------------------------------------

__cold static void chk_verbose_meta(MDBX_chk_scope_t *const scope, const unsigned num) {
  MDBX_chk_line_t *line = chk_line_begin(scope, MDBX_chk_verbose);
  MDBX_chk_internal_t *const chk = scope->internal;
  if (line) {
    MDBX_env *const env = chk->usr->env;
    const bool have_bootid = (chk->envinfo.mi_bootid.current.x | chk->envinfo.mi_bootid.current.y) != 0;
    const bool bootid_match = have_bootid && memcmp(&chk->envinfo.mi_bootid.meta[num], &chk->envinfo.mi_bootid.current,
                                                    sizeof(chk->envinfo.mi_bootid.current)) == 0;

    const char *status = "stay";
    if (num == chk->troika.recent)
      status = "head";
    else if (num == TROIKA_TAIL(&chk->troika))
      status = "tail";
    line = chk_print(line, "meta-%u: %s, ", num, status);

    switch (chk->envinfo.mi_meta_sign[num]) {
    case DATASIGN_NONE:
      line = chk_puts(line, "no-sync/legacy");
      break;
    case DATASIGN_WEAK:
      line = chk_print(line, "weak-%s",
                       have_bootid ? (bootid_match ? "intact (same boot-id)" : "dead") : "unknown (no boot-id)");
      break;
    default:
      line = chk_puts(line, "steady");
      break;
    }
    const txnid_t meta_txnid = chk->envinfo.mi_meta_txnid[num];
    line = chk_print(line, " txn#%" PRIaTXN ", ", meta_txnid);
    if (chk->envinfo.mi_bootid.meta[num].x | chk->envinfo.mi_bootid.meta[num].y)
      line = chk_print(line, "boot-id %" PRIx64 "-%" PRIx64 " (%s)", chk->envinfo.mi_bootid.meta[num].x,
                       chk->envinfo.mi_bootid.meta[num].y, bootid_match ? "live" : "dissimilar");
    else
      line = chk_puts(line, "no boot-id");

    if (env->stuck_meta >= 0) {
      if (num == (unsigned)env->stuck_meta)
        line = chk_print(line, ", %s", "forced for checking");
    } else if (meta_txnid > chk->envinfo.mi_recent_txnid &&
               (env->flags & (MDBX_EXCLUSIVE | MDBX_RDONLY)) == MDBX_EXCLUSIVE)
      line = chk_print(line, ", rolled-back %" PRIu64 " commit(s) (%" PRIu64 " >>> %" PRIu64 ")",
                       meta_txnid - chk->envinfo.mi_recent_txnid, meta_txnid, chk->envinfo.mi_recent_txnid);
    chk_line_end(line);
  }
}

//-----------------------------------------------------------------------------

__cold static int env_chk(MDBX_chk_scope_t *const scope) {
  MDBX_chk_internal_t *const chk = scope->internal;
  MDBX_chk_context_t *const usr = chk->usr;
  MDBX_env *const env = usr->env;
  MDBX_txn *const txn = usr->txn;
  int err = env_info(env, txn, &chk->envinfo, &chk->troika);
  if (unlikely(err))
    return chk_error_rc(scope, err, "env_info");

  MDBX_chk_line_t *line =
      chk_puts(chk_line_begin(scope, MDBX_chk_info - (1 << MDBX_chk_severity_prio_shift)), "dxb-id ");
  if (chk->envinfo.mi_dxbid.x | chk->envinfo.mi_dxbid.y)
    line = chk_print(line, "%016" PRIx64 "-%016" PRIx64, chk->envinfo.mi_dxbid.x, chk->envinfo.mi_dxbid.y);
  else
    line = chk_puts(line, "is absent");
  chk_line_end(line);

  line = chk_puts(chk_line_begin(scope, MDBX_chk_info), "current boot-id ");
  if (chk->envinfo.mi_bootid.current.x | chk->envinfo.mi_bootid.current.y)
    line = chk_print(line, "%016" PRIx64 "-%016" PRIx64, chk->envinfo.mi_bootid.current.x,
                     chk->envinfo.mi_bootid.current.y);
  else
    line = chk_puts(line, "is unavailable");
  chk_line_end(line);

  line = chk_print_size(chk_line_begin(scope, MDBX_chk_verbose), "system unified page cache block ",
                        chk->envinfo.mi_sys_upcblk, "");
  chk_line_end(line);

  env->dxb_mmap.filesize = chk->envinfo.mi_dxb_fsize;

  //--------------------------------------------------------------------------

  err = chk_scope_begin(chk, 1, MDBX_chk_meta, nullptr, &usr->result.problems_meta, "Peek the meta-pages...");
  if (likely(!err)) {
    MDBX_chk_scope_t *const inner = usr->scope;
    const uint64_t dxbfile_pages = env->dxb_mmap.filesize >> env->ps2ln;
    usr->result.alloc_pages = txn->geo.first_unallocated;
    usr->result.backed_pages = bytes2pgno(env, env->dxb_mmap.current);
    if (unlikely(usr->result.backed_pages > dxbfile_pages))
      chk_scope_issue(inner, "backed-pages %zu > dxb-file-pages %" PRIu64, usr->result.backed_pages, dxbfile_pages);
    if (unlikely(usr->result.backed_pages < NUM_METAS)) {
      chk_scope_issue(inner, "backed-pages %zu < num-metas %u", usr->result.backed_pages, NUM_METAS);
      return MDBX_CORRUPTED;
    }
    if (unlikely(usr->result.backed_pages > (size_t)MAX_PAGENO + 1)) {
      chk_scope_issue(inner, "backed-pages %zu > max-pages %zu", usr->result.backed_pages, (size_t)MAX_PAGENO + 1);
      usr->result.backed_pages = MAX_PAGENO + 1;
    }

    if ((env->flags & (MDBX_EXCLUSIVE | MDBX_RDONLY)) != MDBX_RDONLY) {
      if (unlikely(usr->result.backed_pages > dxbfile_pages)) {
        chk_scope_issue(inner, "backed-pages %zu > dxb-file-pages %" PRIu64, usr->result.backed_pages, dxbfile_pages);
        usr->result.backed_pages = (size_t)dxbfile_pages;
      }
      if (unlikely(usr->result.alloc_pages > usr->result.backed_pages)) {
        chk_scope_issue(scope, "alloc-pages %zu > backed-pages %zu", usr->result.alloc_pages, usr->result.backed_pages);
        usr->result.alloc_pages = usr->result.backed_pages;
      }
    } else {
      /* DB may be shrunk by writer down to the allocated (but unused) pages. */
      if (unlikely(usr->result.alloc_pages > usr->result.backed_pages)) {
        chk_scope_issue(inner, "alloc-pages %zu > backed-pages %zu", usr->result.alloc_pages, usr->result.backed_pages);
        usr->result.alloc_pages = usr->result.backed_pages;
      }
      if (unlikely(usr->result.alloc_pages > dxbfile_pages)) {
        chk_scope_issue(inner, "alloc-pages %zu > file-pages %" PRIu64, usr->result.alloc_pages, dxbfile_pages);
        usr->result.alloc_pages = (size_t)dxbfile_pages;
      }
      if (unlikely(usr->result.backed_pages > dxbfile_pages))
        usr->result.backed_pages = (size_t)dxbfile_pages;
    }

    line = chk_line_feed(chk_print(chk_line_begin(inner, MDBX_chk_info), "pagesize %u (%u system), max keysize %u..%u",
                                   env->ps, globals.sys_pagesize, mdbx_env_get_maxkeysize_ex(env, MDBX_DUPSORT),
                                   mdbx_env_get_maxkeysize_ex(env, MDBX_DB_DEFAULTS)));
    if ((env->flags & MDBX_EXCLUSIVE) == 0 && env->lck_mmap.lck) {
      line = chk_line_feed(chk_print(chk_line_begin(inner, MDBX_chk_info), "currently %u readers of %u maximum",
                                     atomic_load32(&env->lck_mmap.lck->rdt_length, mo_Relaxed), env->max_readers));
    }
    line = chk_line_feed(chk_print_size(line, "mapsize ", env->dxb_mmap.current, nullptr));
    if (txn->geo.lower == txn->geo.upper)
      line = chk_print_size(line, "fixed datafile: ", chk->envinfo.mi_geo.current, nullptr);
    else {
      line = chk_print_size(line, "dynamic datafile: ", chk->envinfo.mi_geo.lower, nullptr);
      line = chk_print_size(line, " .. ", chk->envinfo.mi_geo.upper, ", ");
      line = chk_print_size(line, "+", chk->envinfo.mi_geo.grow, ", ");

      line = chk_line_feed(chk_print_size(line, "-", chk->envinfo.mi_geo.shrink, nullptr));
      line = chk_print_size(line, "current datafile: ", chk->envinfo.mi_geo.current, nullptr);
    }
    tASSERT0(txn, txn->geo.now == chk->envinfo.mi_geo.current / chk->envinfo.mi_dxb_pagesize);
    chk_line_end(chk_print(line, ", %u pages", txn->geo.now));
#if IS_WINDOWS || MDBX_DEBUG > 0
    if (txn->geo.shrink_pv && txn->geo.now != txn->geo.upper && scope->verbosity >= MDBX_chk_verbose) {
      line = chk_line_begin(inner, MDBX_chk_notice);
      chk_line_feed(chk_print(line, " > WARNING: Due Windows system limitations a file couldn't"));
      chk_line_feed(chk_print(line, " > be truncated while the database is opened. So, the size"));
      chk_line_feed(chk_print(line, " > database file may be large than the database itself,"));
      chk_line_end(chk_print(line, " > until it will be closed or reopened in read-write mode."));
    }
#endif /* Windows || Debug */
    line = chk_print_size(chk_line_begin(scope, MDBX_chk_verbose), "filesystem: io-block ", chk->envinfo.mi_sys_ioblk,
                          ", space allocated for the dxb-file ");
    if (chk->envinfo.mi_dxb_fallocated == chk->envinfo.mi_geo.current) {
      line = chk_puts(line, "exactly");
    } else {
      line = chk_print_size(
          line, (chk->envinfo.mi_dxb_fallocated > chk->envinfo.mi_geo.current) ? "with excess " : "partially ",
          chk->envinfo.mi_dxb_fallocated, " ");
      ratio2digits_buffer_t buffer;
      line =
          chk_print(line, "%s%%", ratio2percent(chk->envinfo.mi_dxb_fallocated, chk->envinfo.mi_geo.current, &buffer));
    }
    chk_line_end(line);
    chk_verbose_meta(inner, 0);
    chk_verbose_meta(inner, 1);
    chk_verbose_meta(inner, 2);

    if (env->stuck_meta >= 0) {
      chk_line_end(chk_print(chk_line_begin(inner, MDBX_chk_processing),
                             "skip checking meta-pages since the %u"
                             " is selected for verification",
                             env->stuck_meta));
      line = chk_line_feed(chk_print(chk_line_begin(inner, MDBX_chk_resolution),
                                     "transactions: recent %" PRIu64 ", "
                                     "selected for verification %" PRIu64 ", lag %" PRIi64,
                                     chk->envinfo.mi_recent_txnid, chk->envinfo.mi_meta_txnid[env->stuck_meta],
                                     chk->envinfo.mi_recent_txnid - chk->envinfo.mi_meta_txnid[env->stuck_meta]));
      chk_line_end(line);
    } else {
      chk_line_end(chk_puts(chk_line_begin(inner, MDBX_chk_verbose), "performs check for meta-pages clashes"));
      const unsigned meta_clash_mask = meta_eq_mask(&chk->troika);
      if (meta_clash_mask & 1)
        chk_scope_issue(inner, "meta-%d and meta-%d are clashed", 0, 1);
      if (meta_clash_mask & 2)
        chk_scope_issue(inner, "meta-%d and meta-%d are clashed", 1, 2);
      if (meta_clash_mask & 4)
        chk_scope_issue(inner, "meta-%d and meta-%d are clashed", 2, 0);

      const unsigned prefer_steady_metanum = chk->troika.prefer_steady;
      const uint64_t prefer_steady_txnid = chk->troika.txnid[prefer_steady_metanum];
      const unsigned recent_metanum = chk->troika.recent;
      const uint64_t recent_txnid = chk->troika.txnid[recent_metanum];
      if (env->flags & MDBX_EXCLUSIVE) {
        chk_line_end(
            chk_puts(chk_line_begin(inner, MDBX_chk_verbose), "performs full check recent-txn-id with meta-pages"));
        eASSERT0(env, recent_txnid == chk->envinfo.mi_recent_txnid);
        if (prefer_steady_txnid != recent_txnid) {
          if ((chk->flags & MDBX_CHK_READWRITE) != 0 && (env->flags & MDBX_RDONLY) == 0 &&
              recent_txnid > prefer_steady_txnid &&
              (chk->envinfo.mi_bootid.current.x | chk->envinfo.mi_bootid.current.y) != 0 &&
              chk->envinfo.mi_bootid.current.x == chk->envinfo.mi_bootid.meta[recent_metanum].x &&
              chk->envinfo.mi_bootid.current.y == chk->envinfo.mi_bootid.meta[recent_metanum].y) {
            chk_line_end(chk_print(chk_line_begin(inner, MDBX_chk_verbose),
                                   "recent meta-%u is weak, but boot-id match current"
                                   " (will synced upon successful check)",
                                   recent_metanum));
          } else
            chk_scope_issue(inner, "steady meta-%d txn-id mismatch recent-txn-id (%" PRIi64 " != %" PRIi64 ")",
                            prefer_steady_metanum, prefer_steady_txnid, recent_txnid);
        }
      } else if (chk->write_locked) {
        chk_line_end(chk_puts(chk_line_begin(inner, MDBX_chk_verbose),
                              "performs lite check recent-txn-id with meta-pages (not a "
                              "monopolistic mode)"));
        if (recent_txnid != chk->envinfo.mi_recent_txnid) {
          chk_scope_issue(inner, "weak meta-%d txn-id mismatch recent-txn-id (%" PRIi64 " != %" PRIi64 ")",
                          recent_metanum, recent_txnid, chk->envinfo.mi_recent_txnid);
        }
      } else {
        chk_line_end(chk_puts(chk_line_begin(inner, MDBX_chk_verbose),
                              "skip check recent-txn-id with meta-pages (monopolistic or "
                              "read-write mode only)"));
      }

      chk_line_end(chk_print(chk_line_begin(inner, MDBX_chk_resolution),
                             "transactions: recent %" PRIu64 ", latter reader %" PRIu64 ", lag %" PRIi64,
                             chk->envinfo.mi_recent_txnid, chk->envinfo.mi_latter_reader_txnid,
                             chk->envinfo.mi_recent_txnid - chk->envinfo.mi_latter_reader_txnid));
    }
  }
  err = chk_scope_restore(scope, err);

  //--------------------------------------------------------------------------

  const char *const subj_tree = "B-Trees";
  if (chk->flags & MDBX_CHK_SKIP_BTREE_TRAVERSAL)
    chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_processing), "Skipping %s traversal...", subj_tree));
  else {
    err = chk_scope_begin(chk, -1, MDBX_chk_tree, nullptr, &usr->result.tree_problems,
                          "Traversal %s by txn#%" PRIaTXN "...", subj_tree, txn->txnid);
    if (likely(!err))
      err = chk_tree(usr->scope);
    if (usr->result.tree_problems && usr->result.gc_tree_problems == 0)
      usr->result.gc_tree_problems = usr->result.tree_problems;
    if (usr->result.tree_problems && usr->result.kv_tree_problems == 0)
      usr->result.kv_tree_problems = usr->result.tree_problems;
    if (usr->result.histogram_page_age.count) {
      line = chk_line_begin(scope, MDBX_chk_info);
      histogram_dist(line, &usr->result.histogram_page_age, "pages age distribution", "dirty", false);
      chk_line_end(line);
    }
    if (usr->result.histogram_pgno_payload.count) {
      line = chk_line_begin(scope, MDBX_chk_info);
      histogram_dist(line, &usr->result.histogram_pgno_payload, "payload pgno distribution", "bad", false);
      chk_line_end(line);
    }
    if (usr->result.histogram_pgno_retained.count) {
      line = chk_line_begin(scope, MDBX_chk_info);
      histogram_dist(line, &usr->result.histogram_pgno_retained, "retained pgno distribution", "bad", false);
      chk_line_end(line);
    }
    chk_scope_restore(scope, err);
  }

  const char *const subj_gc = chk_v2a(chk, MDBX_CHK_GC);
  if (usr->result.gc_tree_problems > 0)
    chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_processing),
                           "Skip processing %s since %s is corrupted (%" PRIuSIZE " problem(s))", subj_gc, subj_tree,
                           usr->result.problems_gc = usr->result.gc_tree_problems));
  else {
    err = chk_scope_begin(chk, -1, MDBX_chk_gc, &chk->table_gc, &usr->result.problems_gc,
                          "Processing %s by txn#%" PRIaTXN "...", subj_gc, txn->txnid);
    if (likely(!err))
      err = chk_db(usr->scope, FREE_DBI, &chk->table_gc, chk_handle_gc);
    line = chk_line_begin(scope, MDBX_chk_info);
    histogram_print(scope, line, &chk->table_gc.histogram.nested_height_or_gc_span_length, "span(s)", "single", false);
    chk_line_end(line);
    if (usr->result.problems_gc == 0 && (chk->flags & MDBX_CHK_SKIP_BTREE_TRAVERSAL) == 0) {
      const size_t used_pages = usr->result.alloc_pages - usr->result.gc_pages;
      if (usr->result.processed_pages != used_pages)
        chk_scope_issue(usr->scope, "used pages mismatch (%" PRIuSIZE "(walked) != %" PRIuSIZE "(allocated - GC))",
                        usr->result.processed_pages, used_pages);
      if (usr->result.unused_pages != usr->result.gc_pages)
        chk_scope_issue(usr->scope, "GC pages mismatch (%" PRIuSIZE "(expected) != %" PRIuSIZE "(GC))",
                        usr->result.unused_pages, usr->result.gc_pages);
    }
  }
  chk_scope_restore(scope, err);

  //--------------------------------------------------------------------------

  err = chk_scope_begin(chk, 1, MDBX_chk_space, nullptr, nullptr, "Page allocation:");
  const size_t backed = usr->result.backed_pages;
  const size_t boundary = txn->geo.upper;
  const size_t detained = usr->result.gc_pages - usr->result.reclaimable_pages;
  const size_t available2boundary = boundary - usr->result.alloc_pages + usr->result.reclaimable_pages;
  const size_t available2backed = backed - usr->result.alloc_pages + usr->result.reclaimable_pages;
  const size_t remained2boundary = boundary - usr->result.alloc_pages;
  const size_t remained2backed = backed - usr->result.alloc_pages;

  const size_t used = (chk->flags & MDBX_CHK_SKIP_BTREE_TRAVERSAL) ? usr->result.alloc_pages - usr->result.gc_pages
                                                                   : usr->result.processed_pages;

  line = chk_line_begin(usr->scope, MDBX_chk_info);
  line = chk_print_pages_percent(line, "backed by file:\0 of boundary\0", backed, boundary);
  line = chk_print_pages_percent(line, ",\0\0 left to boundary", boundary - backed, boundary);
  line = chk_line_feed(line);

  line = chk_print_pages_percent_bb(line, "used:", used, backed, boundary);
  line = chk_line_feed(line);

  line = chk_print_pages_percent(line, "remained:\0\0 of backed", remained2backed, backed);
  line = chk_print_pages_percent(line, ", left\0\0 to boundary", remained2boundary, boundary);
  line = chk_line_feed(line);

  line = chk_print_pages_percent_bb(line, "reclaimable:", usr->result.reclaimable_pages, backed, boundary);
  line = chk_print_pages_percent_bb(line, ", within GC", usr->result.gc_pages, backed, boundary);
  line = chk_line_feed(line);

  line = chk_print_pages_percent_bb(line, "detained by reader(s):", detained, backed, boundary);
  line = chk_print(line, ", %u reader(s), lag %" PRIi64, chk->envinfo.mi_numreaders,
                   chk->envinfo.mi_recent_txnid - chk->envinfo.mi_latter_reader_txnid);
  line = chk_line_feed(line);

  line = chk_print_pages_percent_bb(line, "allocated:", usr->result.alloc_pages, backed, boundary);
  line = chk_line_feed(line);

  line = chk_print_pages_percent(line, "available:\0 of backed\0", available2backed, backed);
  line = chk_print_pages_percent(line, ", left\0\0 to boundary", available2boundary, boundary);
  chk_line_end(line);

  line = chk_line_begin(usr->scope, MDBX_chk_resolution);
  line = chk_print(line, "%s %zu pages", (boundary == txn->geo.now) ? "total" : "upto", boundary);
  line = chk_print_pages_percent(line, ", backed\0\0", backed, boundary);
  line = chk_print_pages_percent(line, ", allocated\0\0", usr->result.alloc_pages, boundary);
  line = chk_print_pages_percent(line, ", available\0\0", available2boundary, boundary);
  chk_line_end(line);
  chk_scope_restore(scope, err);

  //--------------------------------------------------------------------------

  const char *const subj_main = chk_v2a(chk, MDBX_CHK_MAIN);
  if (chk->flags & MDBX_CHK_SKIP_KV_TRAVERSAL)
    chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_processing), "Skip processing %s...", subj_main));
  else if ((usr->result.problems_kv = usr->result.kv_tree_problems) > 0)
    chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_processing),
                           "Skip processing %s since %s is corrupted (%" PRIuSIZE " problem(s))", subj_main, subj_tree,
                           usr->result.problems_kv = usr->result.kv_tree_problems));
  else {
    err = chk_scope_begin(chk, 0, MDBX_chk_maindb, &chk->table_main, &usr->result.problems_kv, "Processing %s...",
                          subj_main);
    if (likely(!err))
      err = chk_db(usr->scope, MAIN_DBI, &chk->table_main, chk_handle_kv);
    chk_scope_restore(scope, err);

    const char *const subj_tables = "table(s)";
    if (usr->result.problems_kv && usr->result.table_total)
      chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_processing), "Skip processing %s", subj_tables));
    else if (usr->result.problems_kv == 0 && usr->result.table_total == 0)
      chk_line_end(chk_print(chk_line_begin(scope, MDBX_chk_info), "No %s", subj_tables));
    else if (usr->result.problems_kv == 0 && usr->result.table_total) {
      err = chk_scope_begin(chk, 1, MDBX_chk_tables, nullptr, &usr->result.problems_kv,
                            "Processing %s by txn#%" PRIaTXN "...", subj_tables, txn->txnid);
      if (!err)
        err = chk_db(usr->scope, MAIN_DBI, &chk->table_main, nullptr);
      if (usr->scope->subtotal_issues)
        chk_line_end(chk_print(chk_line_begin(usr->scope, MDBX_chk_resolution),
                               "processed %" PRIuSIZE " of %" PRIuSIZE " %s, %" PRIuSIZE " problems(s)",
                               usr->result.table_processed, usr->result.table_total, subj_tables,
                               usr->scope->subtotal_issues));
    }
    chk_scope_restore(scope, err);
  }

  return chk_scope_end(chk, chk_scope_begin(chk, 0, MDBX_chk_conclude, nullptr, nullptr, nullptr));
}

__cold int mdbx_env_chk_encount_problem(MDBX_chk_context_t *ctx) {
  if (likely(ctx && ctx->internal && ctx->internal->usr == ctx && ctx->internal->problem_counter && ctx->scope)) {
    *ctx->internal->problem_counter += 1;
    ctx->scope->subtotal_issues += 1;
    return MDBX_SUCCESS;
  }
  return MDBX_EINVAL;
}

__cold int mdbx_env_chk(MDBX_env *env, const struct MDBX_chk_callbacks *cb, MDBX_chk_context_t *ctx,
                        const MDBX_chk_flags_t flags, MDBX_chk_severity_t verbosity, unsigned timeout_seconds_16dot16) {
  int err, rc = check_env(env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);
  if (unlikely(!cb || !ctx || ctx->internal))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_chk_internal_t *const chk = osal_calloc(1, sizeof(MDBX_chk_internal_t));
  if (unlikely(!chk))
    return LOG_IFERR(MDBX_ENOMEM);

  chk->cb = cb;
  chk->usr = ctx;
  chk->usr->internal = chk;
  chk->usr->env = env;
  chk->flags = flags;

  chk->table_gc.id = -1;
  chk->table_gc.name.iov_base = MDBX_CHK_GC;
  chk->table[FREE_DBI] = &chk->table_gc;

  chk->table_main.id = -1;
  chk->table_main.name.iov_base = MDBX_CHK_MAIN;
  chk->table[MAIN_DBI] = &chk->table_main;

  chk->monotime_timeout =
      timeout_seconds_16dot16 ? osal_16dot16_to_monotime(timeout_seconds_16dot16) + osal_monotime() : 0;
  chk->usr->scope_nesting = 0;
  chk->usr->result.tables = (const void *)&chk->table;

  MDBX_chk_scope_t *const top = chk->scope_stack;
  top->verbosity = verbosity;
  top->internal = chk;

  // init
  rc = chk_scope_end(chk, chk_scope_begin(chk, 0, MDBX_chk_init, nullptr, nullptr, nullptr));

  // lock
  if (likely(!rc))
    rc = chk_scope_begin(chk, 0, MDBX_chk_lock, nullptr, nullptr, "Taking %slock...",
                         (env->flags & (MDBX_RDONLY | MDBX_EXCLUSIVE)) ? "" : "read ");
  if (likely(!rc) && (env->flags & (MDBX_RDONLY | MDBX_EXCLUSIVE)) == 0 && (flags & MDBX_CHK_READWRITE)) {
    rc = mdbx_txn_lock(env, false);
    if (unlikely(rc))
      chk_error_rc(ctx->scope, rc, "mdbx_txn_lock");
    else
      chk->write_locked = true;
  }
  if (likely(!rc)) {
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ctx->txn);
    if (unlikely(rc))
      chk_error_rc(ctx->scope, rc, "mdbx_txn_begin");
  }
  chk_scope_end(chk, rc);

  // doit
  if (likely(!rc)) {
    chk->table_gc.flags = ctx->txn->dbs[FREE_DBI].flags;
    chk->table_main.flags = ctx->txn->dbs[MAIN_DBI].flags;
    rc = env_chk(top);
  }

  // unlock
  if (ctx->txn || chk->write_locked) {
    chk_scope_begin(chk, 0, MDBX_chk_unlock, nullptr, nullptr, nullptr);
    if (ctx->txn) {
      err = mdbx_txn_abort(ctx->txn);
      if (err && !rc)
        rc = err;
      ctx->txn = nullptr;
    }
    if (chk->write_locked)
      mdbx_txn_unlock(env);
    rc = chk_scope_end(chk, rc);
  }

  // finalize
  err = chk_scope_begin(chk, 0, MDBX_chk_finalize, nullptr, nullptr, nullptr);
  rc = chk_scope_end(chk, err ? err : rc);
  chk_dispose(chk);
  return LOG_IFERR(rc);
}
