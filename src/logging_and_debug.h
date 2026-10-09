/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

#ifndef __Wpedantic_format_voidptr
MDBX_MAYBE_UNUSED static inline const void *__Wpedantic_format_voidptr(const void *ptr) { return ptr; }
#define __Wpedantic_format_voidptr(ARG) __Wpedantic_format_voidptr(ARG)
#endif /* __Wpedantic_format_voidptr */

/* --------------------------------------------------------------------------------------------------------------- */

MDBX_PRINTF_ARGS(2, 3) static inline const void *panic_fmt_checker(const void *obj, const char *fmt, ...) {
  (void)fmt;
  return obj;
}

#if MDBX_CHECKING < 0

#define panic(msg_text) __noop
#define panic_obj(obj, msg_text) __noop
#define panic_fmt(obj, msg_text, ...) __noop

#else

struct MDBX_panic_point {
  const char *const function;
  const char *const msg;
  unsigned line;
};

__extern_C MDBX_NORETURN void panic_at(const struct MDBX_panic_point *const at);
__extern_C MDBX_NORETURN void panic_at_obj(const struct MDBX_panic_point *const at, const void *obj);
__extern_C MDBX_NORETURN void panic_at_fmt(const struct MDBX_panic_point *const at, const void *obj, ...);

#define panic(msg_text)                                                                                                \
  do {                                                                                                                 \
    static const char panic_msg[] = msg_text;                                                                          \
    static const struct MDBX_panic_point panic_point = {__func__, panic_msg, __LINE__};                                \
    panic_at(&panic_point);                                                                                            \
  } while (0)

#define panic_obj(obj, msg_text)                                                                                       \
  do {                                                                                                                 \
    static const char panic_msg[] = msg_text;                                                                          \
    static const struct MDBX_panic_point panic_point = {__func__, panic_msg, __LINE__};                                \
    panic_at_obj(&panic_point, obj);                                                                                   \
  } while (0)

#define panic_fmt(obj, msg_text, ...)                                                                                  \
  do {                                                                                                                 \
    static const char panic_msg[] = msg_text;                                                                          \
    static const struct MDBX_panic_point panic_point = {__func__, panic_msg, __LINE__};                                \
    panic_at_fmt(&panic_point, panic_fmt_checker(obj, msg_text, __VA_ARGS__), __VA_ARGS__);                            \
  } while (0)

#endif /* MDBX_CHECKING < 0 */

#define ENSURE_MSG(expr, msg)                                                                                          \
  do {                                                                                                                 \
    if (unlikely(!(expr)))                                                                                             \
      panic(msg);                                                                                                      \
  } while (0)

#define ENSURE_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (unlikely(!(expr)))                                                                                             \
      panic_obj(obj, #expr);                                                                                           \
  } while (0)

#define ENSURE(expr) ENSURE_MSG(expr, #expr)

/* --------------------------------------------------------------------------------------------------------------- */

#if MDBX_CHECKING < 1
#define CHECKS0_ENABLED() (0)
#else
#define CHECKS0_ENABLED() (1)
#endif
#if MDBX_CHECKING < 2
#define CHECKS1_ENABLED() (0)
#else
#define CHECKS1_ENABLED() (globals.runtime_flags & (unsigned)MDBX_DBG_ASSERT)
#endif
#if MDBX_CHECKING < 3
#define CHECKS2_ENABLED() (0)
#else
#define CHECKS2_ENABLED() (unlikely(globals.runtime_flags & (unsigned)MDBX_DBG_AUDIT))
#endif

#if MDBX_DEBUG < 0
#define LOG_ENABLED(LVL) (0)
#elif MDBX_DEBUG > 0
#define LOG_ENABLED(LVL) unlikely(LVL <= globals.loglevel)
#else
#define LOG_ENABLED(LVL) (LVL < MDBX_LOG_VERBOSE && LVL <= globals.loglevel)
#endif /* MDBX_DEBUG */

/* --------------------------------------------------------------------------------------------------------------- */

/* lite-costs checks */
#define CHECK0(expr)                                                                                                   \
  do {                                                                                                                 \
    if (CHECKS0_ENABLED())                                                                                             \
      ENSURE(expr);                                                                                                    \
  } while (0)

#define CHECK0_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (CHECKS0_ENABLED())                                                                                             \
      ENSURE_OBJ(obj, expr);                                                                                           \
  } while (0)

/* medium-costs checks */
#define CHECK1(expr)                                                                                                   \
  do {                                                                                                                 \
    if (CHECKS1_ENABLED())                                                                                             \
      ENSURE(expr);                                                                                                    \
  } while (0)

#define CHECK1_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (CHECKS1_ENABLED())                                                                                             \
      ENSURE_OBJ(obj, expr);                                                                                           \
  } while (0)

/* high-costs checks */
#define CHECK2(expr)                                                                                                   \
  do {                                                                                                                 \
    if (CHECKS2_ENABLED())                                                                                             \
      ENSURE(expr);                                                                                                    \
  } while (0)

#define CHECK2_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (CHECKS2_ENABLED())                                                                                             \
      ENSURE_OBJ(obj, expr);                                                                                           \
  } while (0)

MDBX_MAYBE_UNUSED static inline const void *txn2obj(const MDBX_txn *txn) { return txn; }
MDBX_MAYBE_UNUSED static inline const void *cursor2obj(const MDBX_cursor *mc) { return mc; }
MDBX_MAYBE_UNUSED static inline const void *env2obj(const MDBX_env *env) { return env; }

#define ASSERT(expr) CHECK0(expr)
#define eASSERT0(env, expr) CHECK0_OBJ(env2obj(env), expr)
#define eASSERT1(env, expr) CHECK1_OBJ(env2obj(env), expr)
#define eASSERT2(env, expr) CHECK2_OBJ(env2obj(env), expr)
#define tASSERT0(txn, expr) CHECK0_OBJ(txn2obj(txn), expr)
#define tASSERT1(txn, expr) CHECK1_OBJ(txn2obj(txn), expr)
#define tASSERT2(txn, expr) CHECK2_OBJ(txn2obj(txn), expr)
#define cASSERT0(mc, expr) CHECK0_OBJ(cursor2obj(mc), expr)
#define cASSERT1(mc, expr) CHECK1_OBJ(cursor2obj(mc), expr)
#define cASSERT2(mc, expr) CHECK2_OBJ(cursor2obj(mc), expr)

/* "Deviant-caller" assertions: guard situations that are valid-but-deviant in
 * the CALLER rather than invariant violations of the engine. In dev/test
 * builds (MDBX_PROBES) they become controllable probe-sites (see the
 * probe-bus block below); otherwise they reduce exactly to CHECK0(), i.e. the
 * historical ASSERT() semantics, so dist/amalgamated builds are unchanged. */
#ifndef DEV_ASSERT
#define DEV_ASSERT(expr) CHECK0(expr)
#endif
#ifndef DEV_ASSERT_T
#define DEV_ASSERT_T(tag, expr) CHECK0(expr)
#endif

/* --------------------------------------------------------------------------------------------------------------- */

#ifndef __cplusplus

MDBX_INTERNAL void MDBX_PRINTF_ARGS(4, 5) debug_log(int level, const char *function, int line, const char *fmt, ...)
    MDBX_PRINTF_ARGS(4, 5);
MDBX_INTERNAL void debug_log_va(int level, const char *function, int line, const char *fmt, va_list args);

#define DEBUG_EXTRA(fmt, ...)                                                                                          \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_EXTRA))                                                                                   \
      debug_log(MDBX_LOG_EXTRA, __func__, __LINE__, fmt, __VA_ARGS__);                                                 \
  } while (0)

#define DEBUG_EXTRA_PRINT(fmt, ...)                                                                                    \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_EXTRA))                                                                                   \
      debug_log(MDBX_LOG_EXTRA, nullptr, 0, fmt, __VA_ARGS__);                                                         \
  } while (0)

#define TRACE(fmt, ...)                                                                                                \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_TRACE))                                                                                   \
      debug_log(MDBX_LOG_TRACE, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                            \
  } while (0)

#define DEBUG(fmt, ...)                                                                                                \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_DEBUG))                                                                                   \
      debug_log(MDBX_LOG_DEBUG, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                            \
  } while (0)

#define VERBOSE(fmt, ...)                                                                                              \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_VERBOSE))                                                                                 \
      debug_log(MDBX_LOG_VERBOSE, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                          \
  } while (0)

#define NOTICE(fmt, ...)                                                                                               \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_NOTICE))                                                                                  \
      debug_log(MDBX_LOG_NOTICE, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                           \
  } while (0)

#define WARNING(fmt, ...)                                                                                              \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_WARN))                                                                                    \
      debug_log(MDBX_LOG_WARN, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                             \
  } while (0)

#undef ERROR /* wingdi.h                                                                                               \
  Yeah, morons from M$ put such definition to the public header. */

#define ERROR(fmt, ...)                                                                                                \
  do {                                                                                                                 \
    if (LOG_ENABLED(MDBX_LOG_ERROR))                                                                                   \
      debug_log(MDBX_LOG_ERROR, __func__, __LINE__, fmt "\n", __VA_ARGS__);                                            \
  } while (0)

#define FATAL(fmt, ...) debug_log(MDBX_LOG_FATAL, __func__, __LINE__, fmt "\n", __VA_ARGS__);

MDBX_MAYBE_UNUSED static inline void jitter4testing(bool tiny) {
#if MDBX_DEBUG > 0
  if (globals.runtime_flags & (unsigned)MDBX_DBG_JITTER)
    osal_jitter(tiny);
#else
  (void)tiny;
#endif
}

MDBX_MAYBE_UNUSED MDBX_INTERNAL void page_list(page_t *mp);

MDBX_INTERNAL const char *pagetype_caption(const uint8_t type, char buf4unknown[16]);
/* Key size which fits in a DKBUF (debug key buffer). */
#define DKBUF_MAX 127
#define DKBUF char dbg_kbuf[DKBUF_MAX * 4 + 2]
#define DKEY(x) mdbx_dump_val(x, dbg_kbuf, DKBUF_MAX * 2 + 1)
#define DVAL(x) mdbx_dump_val(x, dbg_kbuf + DKBUF_MAX * 2 + 1, DKBUF_MAX * 2 + 1)

#if MDBX_DEBUG > 0
#define DKBUF_DEBUG DKBUF
#define DKEY_DEBUG(x) DKEY(x)
#define DVAL_DEBUG(x) DVAL(x)
#else
#define DKBUF_DEBUG ((void)(0))
#define DKEY_DEBUG(x) ("-")
#define DVAL_DEBUG(x) ("-")
#endif

MDBX_INTERNAL void log_error(const int err, const char *func, unsigned line);

MDBX_MAYBE_UNUSED static inline int log_if_error(const int err, const char *func, unsigned line) {
  if (unlikely(err != MDBX_SUCCESS))
    log_error(err, func, line);
  return err;
}

#define LOG_IFERR(err) log_if_error((err), __func__, __LINE__)

#endif /* !__cplusplus */

/* --------------------------------------------------------------------------------------------------------------- */

MDBX_MAYBE_UNUSED static inline char sanitizer_kind_of_poison(const void *addr, size_t size) {
  if (ASAN_REGISON_IS_POISONED(addr, size))
    return 'P';
  if (mdbx_running_on_Valgrind()) {
    if (VALGRIND_CHECK_MEM_IS_ADDRESSABLE(addr, size))
      return 'N';
    if (VALGRIND_CHECK_MEM_IS_DEFINED(addr, size))
      return 'U';
  }
  return 0;
}

/* --------------------------------------------------------------------------------------------------------------- */

/* Non-probe fallbacks: keep amalgamated/dist sources compiling even though the
 * probe-bus implementation is dev-only (cut off from the amalgamation). These
 * are zero-cost no-ops unless MDBX_PROBES is defined. */
#ifndef MPROBE_COLLECT
#define MPROBE_COLLECT(name, value) ((void)(name), (void)(value))
#endif
#ifndef MPROBE_WATCH
#define MPROBE_WATCH(name, value) ((void)(name), (void)(value))
#endif
#ifndef MPROBE_FAULT
#define MPROBE_FAULT(name, var) ((void)(name), (void)(var))
#endif

/*> dist-cutoff-begin */
#if defined(MDBX_PROBES)

/* ---------------------------------------------------------------------------
 * Probe-bus (mprobe v2): managed/controlled testing instrumentation.
 *
 * Unified engine-side successor of the former tests/tracing/mprobe v1,
 * adopting the same taxonomy (COLLECT/WATCH/FAULT) and adding:
 *  - a process-wide registry keyed by stable SEMANTIC TAGS (not file:line);
 *  - per-site arm/disarm, word-size counters (seen/hits/suppressed) and
 *    value capture;
 *  - deterministic error injection (return-code at FAULT sites, allocation
 *    failures via the osal_* redirect), driven by the same control channel;
 *  - control from any test either in-process (mprobe_ctl) or via a simple
 *    file IPC (env MDBX_PROBE_CTL=<dir>, files <dir>/cmd and <dir>/rep);
 *  - a single generic DTRACE marker fired at every site (external tracers).
 *
 * Contract & methodology: docs/engineering/testing-methodology.md (ch. 9-10);
 * catalog of existing USDT/DTrace markers: docs/engineering/probes.md.
 * Enabled by the MDBX_PROBES build option (dev-only, OFF in dist). Runtime
 * activation via env MDBX_PROBES=1 or MDBX_PROBE_CTL=<dir>.
 * ------------------------------------------------------------------------- */

enum mprobe_kind {
  mprobe_kind_collect, /* statistics/metrics channel (aggregation) */
  mprobe_kind_watch,   /* observable fact/branch (event channel) */
  mprobe_kind_fault,   /* injection point: may force an error return/value */
  mprobe_kind_assert   /* deviant-caller assertion (DEV_ASSERT) */
};

/* Static per-call-site anchor. The canonical state lives in a process-wide
 * registry record keyed by `name` (semantic tag); the anchor is just cheap
 * metadata used to find-or-create that record on first evaluation. */
struct mprobe_site {
  const char *name;            /* semantic tag (primary key), stable across refactors */
  const char *file;            /* __FILE__ auxiliary */
  unsigned line;               /* __LINE__ auxiliary */
  uint32_t kind;               /* mprobe_kind */
  mdbx_atomic_size_t rec;      /* canonical registry record, resolved lazily */
};

enum mprobe_action_mode {
  mprobe_action_panic = 0,     /* default: report and abort, like ASSERT */
  mprobe_action_log,           /* report via debug_log and continue */
  mprobe_action_count          /* count only, no report/abort */
};

/* Control entry point: applies a request, writes reply into the buffer.
 * Requests are NUL-terminated lines, ops:
 *   list                                     - list all registered sites
 *   arm <tag-pattern> / disarm <tag-pattern> - control individual sites
 *   mode <panic|log|count>                   - global action mode
 *   fault <tag-pattern> <code>|<none>        - inject error return at FAULT sites
 *   alloc-fault <count>|<none>               - fail next N allocations
 *   query <tag-pattern>                      - dump site state
 *   reset <tag-pattern>                      - zero counters
 *   sync                                     - drain file-IPC, then ack
 * Reply format: "ok\n" + optional payload lines, or "err <msg>\n".
 * Tag patterns support a trailing '*', e.g. "rkl_resize:*". */
LIBMDBX_API int mprobe_ctl(const char *request, char *reply, size_t reply_size);

/* Core evaluators called from the macros below (exported for white-box tests
 * that compile engine sources into themselves and link against libmdbx). */
LIBMDBX_API void mprobe_fire(struct mprobe_site *site, intptr_t value);
LIBMDBX_API void mprobe_fault(struct mprobe_site *site, void *var);
LIBMDBX_API void mprobe_assert_failed(struct mprobe_site *site, const char *expr);
LIBMDBX_API void mprobe_assert_ok(struct mprobe_site *site);

#ifndef MDBX_MPROBE_CAT_
#define MDBX_MPROBE_CAT_(a, b) a##b
#endif
#ifndef MDBX_MPROBE_CAT
#define MDBX_MPROBE_CAT(a, b) MDBX_MPROBE_CAT_(a, b)
#endif
#ifndef MDBX_MPROBE_STR_
#define MDBX_MPROBE_STR_(x) #x
#endif
#ifndef MDBX_MPROBE_STR
#define MDBX_MPROBE_STR(x) MDBX_MPROBE_STR_(x)
#endif
#ifndef MDBX_MPROBE_VAR
/* NOTE: __LINE__ (not __COUNTER__) must be used here: within a single macro
 * expansion it evaluates to the SAME call-site value for both the static
 * declaration and the references below, while __COUNTER__ advances per
 * expansion and would desynchronize them. */
#define MDBX_MPROBE_VAR(base) MDBX_MPROBE_CAT(base, __LINE__)
#endif
#ifndef MDBX_MPROBE_AT
#define MDBX_MPROBE_AT __FILE__ ":" MDBX_MPROBE_STR(__LINE__)
#endif

/* Statistics probe: aggregates `value` at the point (count + last seen). */
#undef MPROBE_COLLECT
#define MPROBE_COLLECT(name, value)                                                \
  do {                                                                             \
    static struct mprobe_site MDBX_MPROBE_VAR(mprobe_site_) = {                    \
        #name, __FILE__, __LINE__, mprobe_kind_collect, {0}};                      \
    mprobe_fire(&MDBX_MPROBE_VAR(mprobe_site_), (intptr_t)(value));                \
  } while (0)

/* Event probe: records that the branch/fact was observed. */
#undef MPROBE_WATCH
#define MPROBE_WATCH(name, value)                                                  \
  do {                                                                             \
    static struct mprobe_site MDBX_MPROBE_VAR(mprobe_site_) = {                    \
        #name, __FILE__, __LINE__, mprobe_kind_watch, {0}};                        \
    mprobe_fire(&MDBX_MPROBE_VAR(mprobe_site_), (intptr_t)(value));                \
  } while (0)

/* Injection probe: when a fault rule is armed for this tag, `var` is mutated to
 * the injected error code (e.g. MDBX_TXN_FULL); otherwise a no-op that still
 * observes the site (seen/hits). */
#undef MPROBE_FAULT
#define MPROBE_FAULT(name, var)                                                    \
  do {                                                                             \
    static struct mprobe_site MDBX_MPROBE_VAR(mprobe_site_) = {                    \
        #name, __FILE__, __LINE__, mprobe_kind_fault, {0}};                        \
    mprobe_fault(&MDBX_MPROBE_VAR(mprobe_site_), &(var));                          \
  } while (0)

#undef DEV_ASSERT
#undef DEV_ASSERT_T

/* Deviant-caller assertion built on the probe-bus: in the probe build it is a
 * controllable site (see DEV_ASSERT_T for stable tags); otherwise it reduces
 * to CHECK0() exactly as declared above (dist behavior unchanged). */
#define DEV_ASSERT(expr)                                                           \
  DEV_ASSERT_T(MDBX_MPROBE_STR(expr) "@" __FILE__ ":" MDBX_MPROBE_STR(__LINE__),   \
               expr)
#define DEV_ASSERT_T(tag, expr)                                                    \
  do {                                                                             \
    static struct mprobe_site MDBX_MPROBE_VAR(mprobe_site_) = {(tag), __FILE__,   \
                                                                __LINE__,           \
                                                                mprobe_kind_assert, \
                                                                {0}};              \
    if (unlikely(!(expr)))                                                         \
      mprobe_assert_failed(&MDBX_MPROBE_VAR(mprobe_site_), #expr);                 \
    else                                                                           \
      mprobe_assert_ok(&MDBX_MPROBE_VAR(mprobe_site_));                            \
  } while (0)

#endif /* MDBX_PROBES */
/*< dist-cutoff-end */
