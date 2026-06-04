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

#define ASSERT(expr) CHECK0(expr)
#define eASSERT0(env, expr) CHECK0_OBJ(env, expr)
#define eASSERT1(env, expr) CHECK1_OBJ(env, expr)
#define eASSERT2(env, expr) CHECK2_OBJ(env, expr)
#define tASSERT0(txn, expr) CHECK0_OBJ(txn, expr)
#define tASSERT1(txn, expr) CHECK1_OBJ(txn, expr)
#define tASSERT2(txn, expr) CHECK2_OBJ(txn, expr)
#define cASSERT0(mc, expr) CHECK0_OBJ(mc, expr)
#define cASSERT1(mc, expr) CHECK1_OBJ(mc, expr)
#define cASSERT2(mc, expr) CHECK2_OBJ(mc, expr)

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
