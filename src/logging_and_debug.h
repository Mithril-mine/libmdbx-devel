/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

#ifndef __Wpedantic_format_voidptr
MDBX_MAYBE_UNUSED static inline const void *__Wpedantic_format_voidptr(const void *ptr) { return ptr; }
#define __Wpedantic_format_voidptr(ARG) __Wpedantic_format_voidptr(ARG)
#endif /* __Wpedantic_format_voidptr */

#ifndef __cplusplus

struct MDBX_panic_point {
  const char *const function;
  const char *const msg;
  unsigned line;
};

__extern_C MDBX_NORETURN void panic_at(const struct MDBX_panic_point *const at);
__extern_C MDBX_NORETURN void panic_at_obj(const struct MDBX_panic_point *const at, const void *obj);
__extern_C MDBX_NORETURN void panic_at_fmt(const struct MDBX_panic_point *const at, const void *obj, ...);

MDBX_PRINTF_ARGS(2, 3) static inline const void *panic_fmt_checker(const void *obj, const char *fmt, ...) {
  (void)fmt;
  return obj;
}

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

#define ENSURE_MSG(expr, msg)                                                                                          \
  do {                                                                                                                 \
    if (unlikely(!(expr)))                                                                                             \
      panic(msg);                                                                                                      \
  } while (0)

#define ENSURE(expr) ENSURE_MSG(expr, #expr)

#define ENSURE_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (unlikely(!(expr)))                                                                                             \
      panic_obj(obj, #expr);                                                                                           \
  } while (0)

MDBX_INTERNAL void MDBX_PRINTF_ARGS(4, 5) debug_log(int level, const char *function, int line, const char *fmt, ...)
    MDBX_PRINTF_ARGS(4, 5);
MDBX_INTERNAL void debug_log_va(int level, const char *function, int line, const char *fmt, va_list args);

#if MDBX_DEBUG
#define LOG_ENABLED(LVL) unlikely(LVL <= globals.loglevel)
#define AUDIT_ENABLED() unlikely((globals.runtime_flags & (unsigned)MDBX_DBG_AUDIT))
#else /* MDBX_DEBUG */
#define LOG_ENABLED(LVL) (LVL < MDBX_LOG_VERBOSE && LVL <= globals.loglevel)
#define AUDIT_ENABLED() (0)
#endif /* LOG_ENABLED() & AUDIT_ENABLED() */

#if MDBX_FORCE_ASSERTIONS
#define ASSERT_ENABLED() (1)
#elif MDBX_DEBUG
#define ASSERT_ENABLED() likely((globals.runtime_flags & (unsigned)MDBX_DBG_ASSERT))
#else
#define ASSERT_ENABLED() (0)
#endif /* ASSERT_ENABLED() */

#define ASSERT(expr)                                                                                                   \
  do {                                                                                                                 \
    if (ASSERT_ENABLED())                                                                                              \
      ENSURE(expr);                                                                                                    \
  } while (0)

#define ASSERT_OBJ(obj, expr)                                                                                          \
  do {                                                                                                                 \
    if (ASSERT_ENABLED())                                                                                              \
      ENSURE_OBJ(obj, expr);                                                                                           \
  } while (0)

#define eASSERT(env, expr) ASSERT_OBJ(env, expr)
#define tASSERT(txn, expr) ASSERT_OBJ(txn, expr)
#define cASSERT(mc, expr) ASSERT_OBJ(mc, expr)

#define AUDIT(expr)                                                                                                    \
  do {                                                                                                                 \
    if (AUDIT_ENABLED())                                                                                               \
      ENSURE(expr);                                                                                                    \
  } while (0)

#define AUDIT_OBJ(obj, expr)                                                                                           \
  do {                                                                                                                 \
    if (AUDIT_ENABLED())                                                                                               \
      ENSURE_OBJ(obj, expr);                                                                                           \
  } while (0)

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
#if MDBX_DEBUG
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

#if MDBX_DEBUG
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
