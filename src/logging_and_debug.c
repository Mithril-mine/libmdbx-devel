/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

/*------------------------------------------------------------------------------
 logging */

__cold void debug_log_va(int level, const char *function, int line, const char *fmt, va_list args) {
  ENSURE(osal_fastmutex_acquire(&globals.debug_lock) == 0);
  if (globals.logger.ptr) {
    if (globals.logger_buffer == nullptr)
      globals.logger.fmt(level, function, line, fmt, args);
    else {
      const int len = vsnprintf(globals.logger_buffer, globals.logger_buffer_size, fmt, args);
      if (len > 0)
        globals.logger.nofmt(level, function, line, globals.logger_buffer, len);
    }
  } else {
#if defined(_WIN32) || defined(_WIN64)
    if (IsDebuggerPresent()) {
      int prefix_len = 0;
      char *prefix = nullptr;
      if (function && line > 0)
        prefix_len = osal_asprintf(&prefix, "%s:%d ", function, line);
      else if (function)
        prefix_len = osal_asprintf(&prefix, "%s: ", function);
      else if (line > 0)
        prefix_len = osal_asprintf(&prefix, "%d: ", line);
      if (prefix_len > 0 && prefix) {
        OutputDebugStringA(prefix);
        osal_free(prefix);
      }
      char *msg = nullptr;
      int msg_len = osal_vasprintf(&msg, fmt, args);
      if (msg_len > 0 && msg) {
        OutputDebugStringA(msg);
        osal_free(msg);
      }
    }
#else
    if (function && line > 0)
      fprintf(stderr, "%s:%d ", function, line);
    else if (function)
      fprintf(stderr, "%s: ", function);
    else if (line > 0)
      fprintf(stderr, "%d: ", line);
    vfprintf(stderr, fmt, args);
    fflush(stderr);
#endif
  }
  ENSURE(osal_fastmutex_release(&globals.debug_lock) == 0);
}

__cold void debug_log(int level, const char *function, int line, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  debug_log_va(level, function, line, fmt, args);
  va_end(args);
}

__cold void log_error(const int err, const char *func, unsigned line) {
  ASSERT(err != MDBX_SUCCESS);
  if (unlikely(globals.loglevel >= MDBX_LOG_DEBUG)) {
    const bool is_error = err != MDBX_RESULT_TRUE && err != MDBX_NOTFOUND;
    char buf[256];
    debug_log(is_error ? MDBX_LOG_ERROR : MDBX_LOG_VERBOSE, func, line, "%s %d (%s)\n",
              is_error ? "error" : "condition", err, mdbx_strerror_r(err, buf, sizeof(buf)));
  }
}

/* Dump a val in ascii or hexadecimal. */
__cold const char *mdbx_dump_val(const MDBX_val *val, char *const buf, const size_t bufsize) {
  if (!val)
    return "<null>";
  if (!val->iov_len)
    return "<empty>";
  if (!buf || bufsize < 4)
    return nullptr;

  if (!val->iov_base) {
    int len = snprintf(buf, bufsize, "<nullptr.%zu>", val->iov_len);
    ASSERT(len > 0 && (size_t)len < bufsize);
    (void)len;
    return buf;
  }

  bool is_ascii = true;
  const uint8_t *const data = val->iov_base;
  for (size_t i = 0; i < val->iov_len; i++)
    if (data[i] < ' ' || data[i] > '~') {
      is_ascii = false;
      break;
    }

  if (is_ascii) {
    int len = snprintf(buf, bufsize, "%.*s", (val->iov_len > INT_MAX) ? INT_MAX : (int)val->iov_len, data);
    ASSERT(len > 0 && (size_t)len < bufsize);
    (void)len;
  } else {
    char *const detent = buf + bufsize - 2;
    char *ptr = buf;
    *ptr++ = '<';
    for (size_t i = 0; i < val->iov_len && ptr < detent; i++) {
      const char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
      *ptr++ = hex[data[i] >> 4];
      *ptr++ = hex[data[i] & 15];
    }
    if (ptr < detent)
      *ptr++ = '>';
    *ptr = '\0';
  }
  return buf;
}

__cold static int setup_debug(MDBX_log_level_t level, MDBX_debug_flags_t flags, union logger_union logger, char *buffer,
                              size_t buffer_size) {
  ENSURE(osal_fastmutex_acquire(&globals.debug_lock) == 0);

  const int rc = globals.runtime_flags | (globals.loglevel << 16);
  if (level != MDBX_LOG_DONTCHANGE) {
    level = clamp_unsigned(level, MDBX_LOG_FATAL, (MDBX_DEBUG > 0) ? MDBX_LOG_EXTRA : MDBX_LOG_NOTICE);
    globals.loglevel = (uint8_t)level;
  }

  if (flags != MDBX_DBG_DONTCHANGE) {
    flags &= MDBX_DBG_DUMP | MDBX_DBG_LEGACY_MULTIOPEN | MDBX_DBG_LEGACY_OVERLAP | MDBX_DBG_DONT_UPGRADE
#if MDBX_DEBUG > 0
             | MDBX_DBG_JITTER
#endif
#if MDBX_CHECKING > 1
             | MDBX_DBG_ASSERT
#endif
#if MDBX_CHECKING > 2
             | MDBX_DBG_AUDIT
#endif
        ;
    globals.runtime_flags = (uint8_t)flags;
  }

  ASSERT(MDBX_LOGGER_DONTCHANGE == ((MDBX_debug_func)(intptr_t)-1));
  if (logger.ptr != (void *)((intptr_t)-1)) {
    globals.logger.ptr = logger.ptr;
    globals.logger_buffer = buffer;
    globals.logger_buffer_size = buffer_size;
  }

  ENSURE(osal_fastmutex_release(&globals.debug_lock) == 0);
  return rc;
}

__cold int mdbx_setup_debug_nofmt(MDBX_log_level_t level, MDBX_debug_flags_t flags, MDBX_debug_func_nofmt logger,
                                  char *buffer, size_t buffer_size) {
  union logger_union thunk;
  thunk.nofmt = (logger && buffer && buffer_size) ? logger : MDBX_LOGGER_NOFMT_DONTCHANGE;
  return setup_debug(level, flags, thunk, buffer, buffer_size);
}

__cold int mdbx_setup_debug(MDBX_log_level_t level, MDBX_debug_flags_t flags, MDBX_debug_func logger) {
  union logger_union thunk;
  thunk.fmt = logger;
  return setup_debug(level, flags, thunk, nullptr, 0);
}

/*------------------------------------------------------------------------------
 debug stuff */

__cold const char *pagetype_caption(const uint8_t type, char buf4unknown[16]) {
  switch (type) {
  case P_BRANCH:
    return "branch";
  case P_LEAF:
    return "leaf";
  case P_LEAF | P_SUBP:
    return "subleaf";
  case P_LEAF | P_DUPFIX:
    return "dupfix-leaf";
  case P_LEAF | P_DUPFIX | P_SUBP:
    return "dupfix-subleaf";
  case P_LEAF | P_DUPFIX | P_SUBP | P_LEGACY_DIRTY:
    return "dupfix-subleaf.legacy-dirty";
  case P_LARGE:
    return "large";
  default:
    snprintf(buf4unknown, 16, "unknown_0x%x", type);
    return buf4unknown;
  }
}

__cold static const char *leafnode_type(node_t *n) {
  static const char *const tp[2][2] = {{"", ": DB"}, {": sub-page", ": sub-DB"}};
  return (node_flags(n) & N_BIG) ? ": large page" : tp[!!(node_flags(n) & N_DUP)][!!(node_flags(n) & N_TREE)];
}

/* Display all the keys in the page. */
__cold void page_list(page_t *mp) {
  pgno_t pgno = mp->pgno;
  const char *type;
  node_t *node;
  size_t i, nkeys, nsize, total = 0;
  MDBX_val key;
  DKBUF;

  switch (page_type(mp)) {
  case P_BRANCH:
    type = "Branch page";
    break;
  case P_LEAF:
    type = "Leaf page";
    break;
  case P_LEAF | P_SUBP:
    type = "Leaf sub-page";
    break;
  case P_LEAF | P_DUPFIX:
    type = "Leaf2 page";
    break;
  case P_LEAF | P_DUPFIX | P_SUBP:
    type = "Leaf2 sub-page";
    break;
  case P_LARGE:
    VERBOSE("Overflow page %" PRIaPGNO " pages %u\n", pgno, mp->pages);
    return;
  case P_META:
    VERBOSE("Meta-page %" PRIaPGNO " txnid %" PRIu64 "\n", pgno, unaligned_peek_u64(4, page_meta(mp)->txnid_a));
    return;
  default:
    VERBOSE("Bad page %" PRIaPGNO " flags 0x%X\n", pgno, mp->flags);
    return;
  }

  nkeys = page_numkeys(mp);
  VERBOSE("%s %" PRIaPGNO " numkeys %zu\n", type, pgno, nkeys);

  for (i = 0; i < nkeys; i++) {
    if (is_dupfix_leaf(mp)) { /* DUPFIX pages have no entries[] or node headers */
      key = page_dupfix_key(mp, i, nsize = mp->dupfix_ksize);
      total += nsize;
      VERBOSE("key %zu: nsize %zu, %s\n", i, nsize, DKEY(&key));
      continue;
    }
    node = page_node(mp, i);
    key.iov_len = node_ks(node);
    key.iov_base = node->payload;
    nsize = NODESIZE + key.iov_len;
    if (is_branch(mp)) {
      VERBOSE("key %zu: page %" PRIaPGNO ", %s\n", i, node_pgno(node), DKEY(&key));
      total += nsize;
    } else {
      if (node_flags(node) & N_BIG)
        nsize += sizeof(pgno_t);
      else
        nsize += node_ds(node);
      total += nsize;
      nsize += sizeof(indx_t);
      VERBOSE("key %zu: nsize %zu, %s%s\n", i, nsize, DKEY(&key), leafnode_type(node));
    }
    total = EVEN_CEIL(total);
  }
  VERBOSE("Total: header %u + contents %zu + unused %zu\n", is_dupfix_leaf(mp) ? PAGEHDRSZ : PAGEHDRSZ + mp->lower,
          total, page_room(mp));
}

#if MDBX_CHECKING >= 0

__cold const char *object2class(const void *ptr) {
  if (!ptr)
    return "null";

  int32_t snap_signature = 0;
  if (!osal_safe_peek_uint32(ptr, &snap_signature))
    return "bad";

  switch (snap_signature) {
  case env_signature:
    return "env";
  case txn_signature:
    return "txn";
  case cur_signature_live:
    return "cursor.live";
  case cur_signature_ready4dispose:
    return "cursor.r4clo";
  case cur_signature_wait4eot:
    return "cursor.w4eot";
  }

  return "unknown";
}

MDBX_NORETURN static void fuckup(const char *msg, const char *func, unsigned line, const void *obj) {
  const char *obj_class = object2class(obj);
  MDBX_DTRACE5(panic, func, line, msg, obj_class, obj);
  const MDBX_panic_func panic_func = globals.panic_func;
  if (panic_func)
    panic_func(msg, func, line, obj, obj_class);
  debug_log(MDBX_LOG_FATAL, func, line, obj ? "MDBX-ASSERTION: %s (%s %p)\n" : "MDBX-ASSERTION: %s\n", msg, obj_class,
            (void *)obj);
  osal_panic(msg, func, line);
}

__cold __noinline void panic_at_obj(const struct MDBX_panic_point *const at, const void *obj) {
  fuckup(at->msg, at->function, at->line, obj);
}

__cold __noinline void panic_at(const struct MDBX_panic_point *const at) { panic_at_obj(at, nullptr); }

__cold __noinline void panic_at_fmt(const struct MDBX_panic_point *const at, const void *obj, ...) {
  va_list ap;
  va_start(ap, obj);
  char *message = nullptr;
  const int num = osal_vasprintf(&message, at->msg, ap);
  const char *const const_message = unlikely(num < 1 || !message) ? "<vasprintf() failed>" : message;
  fuckup(const_message, at->function, at->line, obj);
}

__cold void mdbx_assert_fail(const char *msg, const char *func, unsigned line) { fuckup(msg, func, line, nullptr); }

#endif /* MDBX_CHECKING >= 0 */
