/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file osal.h (POSIX part) — platform-specific types and macros of the
/// OSAL layer. Included by osal/osal.h; not for direct inclusion elsewhere.

#pragma once

typedef pthread_t osal_thread_t;
typedef pthread_key_t osal_thread_key_t;
#define INVALID_HANDLE_VALUE (-1)
#define THREAD_CALL
#define THREAD_RESULT void *
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond[2];
} osal_condpair_t;
typedef pthread_mutex_t osal_fastmutex_t;
#define osal_malloc malloc
#define osal_calloc calloc
#define osal_realloc realloc
#define osal_free free
#define osal_strdup strdup

/*> dist-cutoff-begin */
#if defined(MDBX_PROBES)
#define osal_malloc_raw malloc
#define osal_calloc_raw calloc
#define osal_realloc_raw realloc
#define osal_free_raw free
#endif /* MDBX_PROBES */
/*< dist-cutoff-end */

typedef char pathchar_t;
#define MDBX_PRIsPATH "s"
