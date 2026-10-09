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
#if IS_WINDOWS
  } else if (IsDebuggerPresent()) {
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
#endif /* IS_WINDOWS */
  } else {
#if !IS_WINDOWS || !MDBX_WITHOUT_MSVC_CRT
    if (function && line > 0)
      fprintf(stderr, "%s:%d ", function, line);
    else if (function)
      fprintf(stderr, "%s: ", function);
    else if (line > 0)
      fprintf(stderr, "%d: ", line);
    vfprintf(stderr, fmt, args);
    fflush(stderr);
#endif /* !IS_WINDOWS || !MDBX_WITHOUT_MSVC_CRT */
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
  enum { ASCII_PRINTABLE_MIN = 0x20, ASCII_PRINTABLE_MAX = 0x7E };
  const uint8_t *const data = val->iov_base;
  for (size_t i = 0; i < val->iov_len; i++)
    if (data[i] < ASCII_PRINTABLE_MIN || data[i] > ASCII_PRINTABLE_MAX) {
      is_ascii = false;
      break;
    }

  if (is_ascii) {
    /* Outer (int) cast: both branches of the ternary already produce int, but Embarcadero
     * infers the expression type as long; the cast is a no-op on any conforming compiler. */
    int len = snprintf(buf, bufsize, "%.*s", (int)((val->iov_len > INT_MAX) ? INT_MAX : (int)val->iov_len), data);
    if (unlikely(len < 0))
      buf[0] = '\0';
    else if (unlikely((size_t)len >= bufsize))
      buf[bufsize - 1] = '\0';
    else
      ASSERT(len > 0);
  } else {
    const char alpha_offset = 'a' - '9' - 1;
    char *const detent = buf + bufsize - 2;
    char *ptr = buf;
    *ptr++ = '<';
    for (size_t i = 0; i < val->iov_len && ptr < detent; i++) {
      const int8_t hi = data[i] >> 4;
      const int8_t lo = data[i] & 15;
      ptr[0] = (char)('0' + hi + (((9 - hi) >> 7) & alpha_offset));
      ptr[1] = (char)('0' + lo + (((9 - lo) >> 7) & alpha_offset));
      ptr += 2;
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
  ASSERT(rc >= 0);
  return rc;
}

__cold int mdbx_setup_debug_nofmt(MDBX_log_level_t level, MDBX_debug_flags_t flags, MDBX_debug_func_nofmt logger,
                                  char *buffer, size_t buffer_size) {
  union logger_union thunk;
  thunk.nofmt = logger ? logger : MDBX_LOGGER_NOFMT_DONTCHANGE;
  const bool logger_changed = (thunk.nofmt != MDBX_LOGGER_NOFMT_DONTCHANGE);
  const bool buffer_configured = (buffer != nullptr && buffer_size != 0);
  if (unlikely(logger_changed != buffer_configured))
    return -1;
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
  if (!osal_safe_peek_int32(ptr, &snap_signature))
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

MDBX_NORETURN static void panic_internal(const char *msg, const char *func, unsigned line, const void *obj) {
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
  panic_internal(at->msg, at->function, at->line, obj);
}

__cold __noinline void panic_at(const struct MDBX_panic_point *const at) { panic_at_obj(at, nullptr); }

__cold __noinline void panic_at_fmt(const struct MDBX_panic_point *const at, const void *obj, ...) {
  va_list ap;
  va_start(ap, obj);
  char *message = nullptr;
  const int num = osal_vasprintf(&message, at->msg, ap);
  va_end(ap);
  const char *const final_message = unlikely(num < 1 || !message) ? "<vasprintf() failed>" : message;
  panic_internal(final_message, at->function, at->line, obj);
  __unreachable();
}

__cold void mdbx_assert_fail(const char *msg, const char *func, unsigned line) {
  panic_internal(msg, func, line, nullptr);
}

#endif /* MDBX_CHECKING >= 0 */

/*----------------------------------------------------------------------------*/

static inline const char *sanitizer_probe_page_dangling(const MDBX_txn *txn, const MDBX_cursor *const mc,
                                                        const page_t *mp, bool allow_subpage) {
  /* Checking the page structure or header fields is generally inappropriate here, since the function can be called
   * during modification of the b-tree structure, when there may be temporary and incomplete pages on the cursor stacks.
   */
  if (!mp)
    return "null-address";
  const char poison = sanitizer_kind_of_poison(mp, PAGEHDRSZ);
  switch (poison) {
  case 'P':
    return "ASAN.poisoned";
  case 'N':
    return "MEMCHECK.non-addressable";
  case 'U':
    return "MEMCHECK.undefined";
  default:
    return "SANITIZER.other-poison";
  case 0:
    break;
  }

  const size_t mmap_offset = ptr_dist(mp, txn->env->dxb_mmap.base);
  if (mmap_offset < txn->env->dxb_mmap.limit) {
    /* mp in the mapped region */
    size_t pgno = bytes2pgno(txn->env, mmap_offset);
    if (pgno < NUM_METAS || pgno >= txn->geo.first_unallocated)
      return "MMAP.outside-allocation-range";
    if (!allow_subpage && (mmap_offset & (txn->env->ps - 1)) != 0)
      return "unexpected-suppage";
    return nullptr;
  }

#if MDBX_DEBUG_SPILLING > 0
  for (unsigned i = 0; i < mc->tmp_split_top; ++i)
    if (mc->tmp_split[i] == mp)
      return nullptr;
#else
  (void)mc;
#endif /* MDBX_DEBUG_SPILLING */

  if ((txn->flags & MDBX_WRITEMAP) != 0 || !txn->wr.dirtylist)
    return "MMAP.outside-mmap-region";

  do {
    for (size_t i = 1; i <= txn->wr.dirtylist->length; ++i) {
      const size_t dirty_offset = ptr_dist(mp, txn->wr.dirtylist->items[i].ptr);
      if (dirty_offset >= txn->env->ps)
        continue;
      if (dirty_offset && !allow_subpage)
        return "unexpected-suppage";
      return nullptr;
    }
    txn = txn->parent;
  } while (txn);

  return "TXN.outside-dirty-pages";
}

__cold MDBX_ATTRIBUTE_NO_SANITIZE_ADDRESS(MDBX_NOTHING) void cursor_stack(const MDBX_cursor *const mc, const char *func,
                                                                          unsigned line, const char *prefix) {
  MDBX_log_level_t lvl = MDBX_LOG_VERBOSE - 1;
  if (LOG_ENABLED(lvl)) {
    debug_log(lvl, func, line, "cursor%s-%p[%i, flags 0x%X]", prefix, __Wpedantic_format_voidptr(mc), mc->top,
              (uint8_t)mc->flags);
    for (intptr_t i = 0, last = mc->top + mc->stash; i <= last; ++i) {
      char page_flags[16], *pf = page_flags;
      const page_t *mp = mc->pg[i];
      const char *sanitizer_probe = sanitizer_probe_page_dangling(mc->txn, mc, mp, i == 0 && is_inner(mc));
      if (sanitizer_probe) {
        *pf++ = '#';
        *pf++ = '>';
        VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE(mp, sizeof(*mp));
      } else {
        if (is_branch(mp))
          *pf++ = 'B';
        if (is_leaf(mp))
          *pf++ = 'L';
        if (is_dupfix_leaf(mp))
          *pf++ = 'F';
        if (is_subpage(mp))
          *pf++ = 'S';
        *pf++ = '_';

        if (page_check(mc, mp) != MDBX_SUCCESS)
          *pf++ = '%';
        if (!is_correct(mc->txn, mp))
          *pf++ = '!';
        if (is_frozen(mc->txn, mp))
          *pf++ = 'f';
        if (is_shadowed(mc->txn, mp))
          *pf++ = 'h';
        if (is_spilled(mc->txn, mp))
          *pf++ = 's';
        if (is_modifiable(mc->txn, mp))
          *pf++ = 'm';
      }
      *pf = 0;
      debug_log(lvl, nullptr, 0, "%s%zu->%u.%p_%s:%u%s", i ? ", " : "", i, sanitizer_probe ? 0 : mp->pgno,
                __Wpedantic_format_voidptr(mp), page_flags, mc->ki[i], sanitizer_probe ? "\n" : "");
      if (sanitizer_probe) {
#if !IS_WINDOWS || !MDBX_WITHOUT_MSVC_CRT
        fflush(nullptr);
#endif
        ASAN_DESCRIBE_ADDRESS(mp);
        VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE(mp, sizeof(*mp));
      }
    }
    debug_log(lvl, nullptr, 0, "\n");
  }
}

__hot void txn_probe_dbi_cursors_stacks(const MDBX_txn *txn, size_t dbi, const char *func, unsigned line) {
  for (const MDBX_cursor *mc = txn->cursors[dbi]; mc; mc = mc->next) {
    const MDBX_cursor *mx = mc;
    while (!is_poor(mx)) {
      for (intptr_t i = 0, last = mx->top + mx->stash; i <= last; ++i) {
        page_t *mp = mx->pg[i];
        const char *cause = sanitizer_probe_page_dangling(txn, mx, mp, i == 0 && is_inner(mx));
        if (unlikely(cause)) {
          cursor_stack(mc, func, line, ".outer");
          if (mx != mc)
            cursor_stack(mx, func, line, ".inner");
          /* Using the page_check() is mostly invalid here, since the page is known to be dangling,
           * but hope this could help debugging. */
          cASSERT0(mc, mp && page_check(mx, mp) == MDBX_SUCCESS);

          panic_fmt(mc,
                    "Dangling reference from the cursor's stack to a freed page is detected: %s-cursor %p dbi %zu, "
                    "top %i, stash %i, at level %zi, page %p, cause %s",
                    is_inner(mx) ? "inner" : "outer", __Wpedantic_format_voidptr(mx), cursor_dbi(mx), mx->top,
                    mx->stash, i, __Wpedantic_format_voidptr(mp), cause);
        }
      }
      if (!outer_on_duptree_and_inner_pointed(mx))
        break;
      mx = &mx->subcur->cursor;
    }
  }
}

__hot void txn_probe_all_cursors_against_dangling(MDBX_txn *txn, const char *func, unsigned line) {
  TXN_FOREACH_DBI_ALL(txn, dbi) { txn_probe_dbi_cursors_stacks(txn, dbi, func, line); }
}

/*> dist-cutoff-begin */
#if defined(MDBX_PROBES)

/* ---------------------------------------------------------------------------
 * Probe-bus implementation (mprobe v2). See logging_and_debug.h for the
 * contract and the control protocol. All symbols in this block are exported
 * (LIBMDBX_API) so tests can link against them, including white-box tests
 * that compile engine sources into themselves.
 * ------------------------------------------------------------------------- */

#define MPROBE_MAX_SITES 512

/* Canonical per-tag registry record: all anchors with the same semantic tag
 * resolve to the SAME record, so white-box copies of engine code (compiled
 * both into libmdbx and into a test binary) share counters and control. */
struct mprobe_record {
  const char *name; /* semantic tag (primary key) */
  const char *file; /* __FILE__ of the first registration */
  unsigned line;
  uint32_t kind;
  mdbx_atomic_uint32_t flags;        /* bit0: ARMED, bit1: FAULT_ONCE */
  mdbx_atomic_size_t seen;           /* evaluated at least once */
  mdbx_atomic_size_t hits;           /* fires */
  mdbx_atomic_size_t suppressed;     /* would-fire but disarmed */
  mdbx_atomic_size_t value;          /* last captured value */
  mdbx_atomic_size_t fault_code;     /* injected code or 0 */
  struct mprobe_record *next;
};

enum mprobe_record_flags {
  MPROBE_FLAG_ARMED = 1u << 0,
  MPROBE_FLAG_FAULT_ONCE = 1u << 1
};

/* Global runtime state (single registry per process). Guarded by
 * mprobe_reg_lock for structural mutations; counters are word-size atomics. */
static struct mprobe_record *mprobe_registry;
static mdbx_atomic_uint32_t mprobe_reg_lock;
static mdbx_atomic_uint32_t mprobe_nsites;
static mdbx_atomic_uint32_t mprobe_state;       /* bit0: ACTIVE, bit1: IPC */
static mdbx_atomic_uint32_t mprobe_action_mode; /* mprobe_action_mode */
static mdbx_atomic_size_t mprobe_alloc_fault;   /* remaining forced alloc failures */
static char *mprobe_ipc_dir;
static mdbx_atomic_uint64_t mprobe_ipc_offset;

enum mprobe_state_bits {
  MPROBE_STATE_INIT_DONE = 1u << 0,
  MPROBE_STATE_ACTIVE = 1u << 1,
  MPROBE_STATE_IPC = 1u << 2
};

static void mprobe_lock(void) {
  while (unlikely(!atomic_cas32(&mprobe_reg_lock, 0, 1)))
    osal_yield();
}
static void mprobe_unlock(void) { atomic_store32(&mprobe_reg_lock, 0, mo_AcquireRelease); }

/* Lazily read the runtime activation environment. Called at most once per
 * process (guarded by MPROBE_STATE_INIT_DONE under mprobe_lock). */
static void mprobe_init_activation(void) {
  if (atomic_load32(&mprobe_state, mo_Relaxed) & MPROBE_STATE_INIT_DONE)
    return;
  mprobe_lock();
  if (!(atomic_load32(&mprobe_state, mo_Relaxed) & MPROBE_STATE_INIT_DONE)) {
    uint32_t state = MPROBE_STATE_INIT_DONE;
    const char *flag = osal_getenv_singlethreaded("MDBX_PROBES", false);
    const char *dir = osal_getenv_singlethreaded("MDBX_PROBE_CTL", false);
    if (flag && flag[0] && flag[0] != '0')
      state |= MPROBE_STATE_ACTIVE;
    if (dir && dir[0]) {
      mprobe_ipc_dir = osal_strdup(dir);
      if (mprobe_ipc_dir)
        state |= MPROBE_STATE_ACTIVE | MPROBE_STATE_IPC;
    }
    atomic_store32(&mprobe_state, state, mo_AcquireRelease);
    MDBX_DTRACE2(mprobe_active, flag ? flag : "noenv", dir ? dir : "noctl");
  }
  mprobe_unlock();
}

static __always_inline bool mprobe_is_active(void) {
  if (unlikely(!(atomic_load32(&mprobe_state, mo_Relaxed) & MPROBE_STATE_INIT_DONE)))
    mprobe_init_activation();
  return (atomic_load32(&mprobe_state, mo_Relaxed) & MPROBE_STATE_ACTIVE) != 0;
}

static bool mprobe_match(const char *pattern, const char *name) {
  size_t len = strlen(pattern);
  if (len > 0 && pattern[len - 1] == '*')
    return strncmp(name, pattern, len - 1) == 0;
  return strcmp(name, pattern) == 0;
}

static struct mprobe_record *mprobe_find_or_create(const struct mprobe_site *site) {
  struct mprobe_record *rec;
  mprobe_lock();
  for (rec = mprobe_registry; rec; rec = rec->next) {
    if (strcmp(rec->name, site->name) == 0)
      break;
  }
  if (!rec) {
    const uint32_t nsites = atomic_load32(&mprobe_nsites, mo_Relaxed);
    if (nsites < MPROBE_MAX_SITES) {
      rec = osal_calloc_raw(1, sizeof(*rec));
      if (rec) {
        rec->name = site->name;
        rec->file = site->file;
        rec->line = site->line;
        rec->kind = site->kind;
        atomic_store32(&rec->flags, MPROBE_FLAG_ARMED, mo_Relaxed);
        rec->next = mprobe_registry;
        mprobe_registry = rec;
        atomic_add32(&mprobe_nsites, 1);
      }
    }
  }
  mprobe_unlock();
  return rec;
}

/* Resolve the canonical record for an anchor, caching it in the anchor so the
 * hot path is a single relaxed load once registered. */
static struct mprobe_record *mprobe_resolve(struct mprobe_site *site) {
  size_t cached = atomic_load_size(&site->rec, mo_Relaxed);
  if (likely(cached != 0))
    return (struct mprobe_record *)(uintptr_t)cached;
  struct mprobe_record *rec = mprobe_find_or_create(site);
  if (rec)
    atomic_store_size(&site->rec, (size_t)(uintptr_t)rec, mo_AcquireRelease);
  return rec;
}

static void mprobe_emit(const struct mprobe_record *rec) {
  (void)rec;
  MDBX_DTRACE3(mprobe_event, rec->name, rec->file, rec->line);
}

/* Forward decls for the file-IPC drain (see below). */
static void mprobe_drain_ipc(void);
static int mprobe_ctl_apply(const char *request, char *reply, size_t reply_size);

void mprobe_fire(struct mprobe_site *site, intptr_t value) {
  if (unlikely(!mprobe_is_active()))
    return;
  mprobe_drain_ipc();
  struct mprobe_record *rec = mprobe_resolve(site);
  if (unlikely(!rec))
    return;
  atomic_add_size(&rec->seen, 1);
  atomic_add_size(&rec->hits, 1);
  atomic_store_size(&rec->value, (size_t)(intptr_t)value, mo_Relaxed);
  mprobe_emit(rec);
}

void mprobe_fault(struct mprobe_site *site, void *var) {
  if (unlikely(!mprobe_is_active()))
    return;
  mprobe_drain_ipc();
  struct mprobe_record *rec = mprobe_resolve(site);
  if (unlikely(!rec))
    return;
  atomic_add_size(&rec->seen, 1);
  const uint32_t flags = atomic_load32(&rec->flags, mo_Relaxed);
  const size_t code = atomic_load_size(&rec->fault_code, mo_Relaxed);
  if ((flags & MPROBE_FLAG_ARMED) && code != 0) {
    /* Inject the error: mutate the target variable as an int. */
    *(int *)var = (int)(intptr_t)code;
    atomic_add_size(&rec->hits, 1);
    if (flags & MPROBE_FLAG_FAULT_ONCE) {
      atomic_store_size(&rec->fault_code, 0, mo_Relaxed);
      atomic_store32(&rec->flags, flags & ~MPROBE_FLAG_ARMED, mo_Relaxed);
    }
    mprobe_emit(rec);
  } else if (flags & MPROBE_FLAG_ARMED) {
    atomic_add_size(&rec->hits, 1);
  } else {
    atomic_add_size(&rec->suppressed, 1);
  }
}

void mprobe_assert_failed(struct mprobe_site *site, const char *expr) {
  if (unlikely(!mprobe_is_active())) {
    /* Not activated: keep historical ASSERT (CHECK0) semantics. */
    if (CHECKS0_ENABLED()) {
      const struct MDBX_panic_point at = {site->file ? site->file : __func__, expr, site->line};
      panic_at(&at);
    }
    return;
  }
  mprobe_drain_ipc();
  struct mprobe_record *rec = mprobe_resolve(site);
  if (unlikely(!rec))
    return;
  atomic_add_size(&rec->seen, 1);
  if (atomic_load32(&rec->flags, mo_Relaxed) & MPROBE_FLAG_ARMED) {
    atomic_add_size(&rec->hits, 1);
    const uint32_t mode = atomic_load32(&mprobe_action_mode, mo_Relaxed);
    if (mode != mprobe_action_count) {
      debug_log(MDBX_LOG_ERROR, site->file, site->line,
                "DEV_ASSERT failed: %s (tag \"%s\")\n", expr, rec->name);
      if (mode == mprobe_action_panic) {
        const struct MDBX_panic_point at = {site->file ? site->file : __func__, expr, site->line};
        panic_at(&at);
      }
    }
    mprobe_emit(rec);
  } else {
    atomic_add_size(&rec->suppressed, 1);
  }
}

void mprobe_assert_ok(struct mprobe_site *site) {
  if (unlikely(!mprobe_is_active()))
    return;
  mprobe_drain_ipc();
  struct mprobe_record *rec = mprobe_resolve(site);
  if (unlikely(!rec))
    return;
  atomic_add_size(&rec->seen, 1);
}

/* Allocation wrappers used by the osal_* redirect under MDBX_PROBES. */
void *mprobe_alloc(size_t bytes, const char *site) {
  (void)site;
  if (unlikely(!mprobe_is_active()))
    return osal_malloc_raw(bytes);
  mprobe_drain_ipc();
  const size_t remaining = atomic_load_size(&mprobe_alloc_fault, mo_Relaxed);
  if (remaining) {
    atomic_store_size(&mprobe_alloc_fault, remaining - 1, mo_Relaxed);
    return nullptr;
  }
  return osal_malloc_raw(bytes);
}

void *mprobe_realloc(void *ptr, size_t bytes, const char *site) {
  (void)site;
  if (unlikely(!mprobe_is_active()))
    return osal_realloc_raw(ptr, bytes);
  mprobe_drain_ipc();
  const size_t remaining = atomic_load_size(&mprobe_alloc_fault, mo_Relaxed);
  if (remaining) {
    atomic_store_size(&mprobe_alloc_fault, remaining - 1, mo_Relaxed);
    return nullptr;
  }
  return osal_realloc_raw(ptr, bytes);
}

void *mprobe_calloc(size_t nelem, size_t size, const char *site) {
  (void)site;
  if (unlikely(!mprobe_is_active()))
    return osal_calloc_raw(nelem, size);
  mprobe_drain_ipc();
  const size_t remaining = atomic_load_size(&mprobe_alloc_fault, mo_Relaxed);
  if (remaining) {
    atomic_store_size(&mprobe_alloc_fault, remaining - 1, mo_Relaxed);
    return nullptr;
  }
  return osal_calloc_raw(nelem, size);
}

/* --- file IPC ------------------------------------------------------------- */

/* The probe-bus serves the same line protocol over a simple file-based IPC:
 *   env MDBX_PROBE_CTL=<dir>  ->  requests in <dir>/cmd, replies in <dir>/rep
 * The library drains <dir>/cmd lazily (on probe fires and on every mprobe_ctl
 * call, see mprobe_drain_ipc), appends each reply to <dir>/rep. Tests use the
 * "sync" op as a deterministic barrier: write cmd lines, ctl("sync"), act.
 * Plain stdio is used on purpose here (dev-only instrumentation; the engine
 * itself avoids it). The MDBX_WITHOUT_MSVC_CRT build has no CRT stdio, so the
 * file-IPC degrades to no-op there (in-process mprobe_ctl still works). */
#define MPROBE_IPC_LINE_MAX 256

#if !MDBX_WITHOUT_MSVC_CRT
static void mprobe_drain_ipc(void) {
  if (unlikely(!(atomic_load32(&mprobe_state, mo_Relaxed) & MPROBE_STATE_IPC)))
    return;
  char path[1024];
  /* <dir>/cmd: one request per line, appended by the test. */
  int n = snprintf(path, sizeof(path), "%s/cmd", mprobe_ipc_dir);
  if (n <= 0 || (size_t)n >= sizeof(path))
    return;
  FILE *cmd = fopen(path, "rb");
  if (!cmd)
    return;
  const uint64_t offset = atomic_load64(&mprobe_ipc_offset, mo_Relaxed);
  if (offset > 0 && offset < (uint64_t)LONG_MAX && fseek(cmd, (long)offset, SEEK_SET) != 0) {
    fclose(cmd);
    return;
  }
  char request[MPROBE_IPC_LINE_MAX];
  while (fgets(request, sizeof(request), cmd)) {
    const size_t len = strlen(request);
    if (len > 0 && request[len - 1] == '\n')
      request[len - 1] = 0;
    char reply[MPROBE_IPC_LINE_MAX];
    reply[0] = 0;
    if (mprobe_ctl_apply(request, reply, sizeof(reply)) != 0)
      snprintf(reply, sizeof(reply), "err unknown command\n");
    /* <dir>/rep: append the reply. */
    n = snprintf(path, sizeof(path), "%s/rep", mprobe_ipc_dir);
    if (n > 0 && (size_t)n < sizeof(path)) {
      FILE *rep = fopen(path, "ab");
      if (rep) {
        fwrite(reply, 1, strlen(reply), rep);
        fclose(rep);
      }
    }
  }
  const long pos = ftell(cmd);
  fclose(cmd);
  if (pos > 0)
    atomic_store64(&mprobe_ipc_offset, (uint64_t)pos, mo_AcquireRelease);
}
#else /* MDBX_WITHOUT_MSVC_CRT */
static void mprobe_drain_ipc(void) { (void)mprobe_ipc_offset; }
#endif /* MDBX_WITHOUT_MSVC_CRT */

/* --- control protocol ------------------------------------------------------ */

static char *mprobe_reply(char *out, const char *const end, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out, (size_t)(end - out), fmt, args);
  va_end(args);
  if (n < 0)
    return nullptr;
  out += (n < (int)(end - out)) ? n : (int)(end - out);
  return out;
}

static int mprobe_ctl_apply(const char *request, char *reply, size_t reply_size) {
  char *const end = reply + reply_size;
  char *out = reply;

  char op[16];
  const char *p = request;
  while (*p == ' ')
    p++;
  const char *space = strchr(p, ' ');
  const size_t oplen = space ? (size_t)(space - p) : strlen(p);
  if (oplen >= sizeof(op))
    return -1;
  memcpy(op, p, oplen);
  op[oplen] = 0;
  const char *arg = space ? space + 1 : "";

  if (strcmp(op, "list") == 0) {
    mprobe_init_activation();
    out = mprobe_reply(out, end, "ok\n");
    if (!out)
      return -1;
    mprobe_lock();
    for (const struct mprobe_record *rec = mprobe_registry; rec; rec = rec->next) {
      out = mprobe_reply(out, end, "%s\n", rec->name);
      if (!out)
        break;
    }
    mprobe_unlock();
  } else if (strcmp(op, "sync") == 0) {
    mprobe_init_activation();
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "mode") == 0) {
    uint32_t mode;
    if (strcmp(arg, "panic") == 0)
      mode = mprobe_action_panic;
    else if (strcmp(arg, "log") == 0)
      mode = mprobe_action_log;
    else if (strcmp(arg, "count") == 0)
      mode = mprobe_action_count;
    else
      return -1;
    atomic_store32(&mprobe_action_mode, mode, mo_AcquireRelease);
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "alloc-fault") == 0) {
    if (strcmp(arg, "none") == 0 || arg[0] == 0) {
      atomic_store_size(&mprobe_alloc_fault, 0, mo_AcquireRelease);
    } else {
      char *endptr;
      const unsigned long long n = strtoull(arg, &endptr, 10);
      if (endptr == arg || *endptr)
        return -1;
      atomic_store_size(&mprobe_alloc_fault, (size_t)n, mo_AcquireRelease);
    }
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "reset") == 0) {
    mprobe_init_activation();
    mprobe_lock();
    for (struct mprobe_record *rec = mprobe_registry; rec; rec = rec->next) {
      if (mprobe_match(arg, rec->name)) {
        atomic_store_size(&rec->seen, 0, mo_Relaxed);
        atomic_store_size(&rec->hits, 0, mo_Relaxed);
        atomic_store_size(&rec->suppressed, 0, mo_Relaxed);
      }
    }
    mprobe_unlock();
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "arm") == 0 || strcmp(op, "disarm") == 0) {
    mprobe_init_activation();
    mprobe_lock();
    const bool arm = (op[0] == 'a');
    for (struct mprobe_record *rec = mprobe_registry; rec; rec = rec->next) {
      if (mprobe_match(arg, rec->name)) {
        uint32_t flags = atomic_load32(&rec->flags, mo_Relaxed);
        if (arm)
          flags |= MPROBE_FLAG_ARMED;
        else
          flags &= ~MPROBE_FLAG_ARMED;
        atomic_store32(&rec->flags, flags, mo_AcquireRelease);
      }
    }
    mprobe_unlock();
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "fault") == 0) {
    mprobe_init_activation();
    /* fault <pattern> <code>|<none> */
    const char *sp2 = strchr(arg, ' ');
    if (!sp2)
      return -1;
    char pattern[128];
    const size_t plen = (size_t)(sp2 - arg);
    if (plen >= sizeof(pattern))
      return -1;
    memcpy(pattern, arg, plen);
    pattern[plen] = 0;
    const char *code_str = sp2 + 1;
    mprobe_lock();
    for (struct mprobe_record *rec = mprobe_registry; rec; rec = rec->next) {
      if (!mprobe_match(pattern, rec->name))
        continue;
      if (strcmp(code_str, "none") == 0) {
        atomic_store_size(&rec->fault_code, 0, mo_AcquireRelease);
      } else {
        char *endptr;
        const long code = strtol(code_str, &endptr, 0);
        if (endptr == code_str || *endptr)
          continue;
        atomic_store_size(&rec->fault_code, (size_t)(intptr_t)code, mo_AcquireRelease);
      }
    }
    mprobe_unlock();
    out = mprobe_reply(out, end, "ok\n");
  } else if (strcmp(op, "query") == 0) {
    mprobe_init_activation();
    mprobe_lock();
    out = mprobe_reply(out, end, "ok\n");
    for (struct mprobe_record *rec = mprobe_registry; rec; rec = rec->next) {
      if (!mprobe_match(arg, rec->name))
        continue;
      out = mprobe_reply(out, end, "site %s %u %u %zu %zu %zu %zu %s:%u\n", rec->name, rec->kind,
                         (unsigned)(atomic_load32(&rec->flags, mo_Relaxed) & MPROBE_FLAG_ARMED),
                         atomic_load_size(&rec->seen, mo_Relaxed), atomic_load_size(&rec->hits, mo_Relaxed),
                         atomic_load_size(&rec->suppressed, mo_Relaxed), atomic_load_size(&rec->value, mo_Relaxed),
                         rec->file ? rec->file : "", rec->line);
      if (!out)
        break;
    }
    mprobe_unlock();
  } else {
    return -1;
  }
  return (out) ? 0 : -1;
}

int mprobe_ctl(const char *request, char *reply, size_t reply_size) {
  if (!reply || reply_size < 2)
    return -1;
  reply[0] = 0;
  mprobe_init_activation();
  mprobe_drain_ipc();
  if (mprobe_ctl_apply(request, reply, reply_size) != 0) {
    snprintf(reply, reply_size, "err unknown command\n");
    return -1;
  }
  return 0;
}

#endif /* MDBX_PROBES */
/*< dist-cutoff-end */
