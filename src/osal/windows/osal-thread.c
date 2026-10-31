/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-thread.c (Windows part) — condition pairs, fast mutexes and
/// threads.

#include "internals.h"

#ifndef osal_memalign_alloc
int osal_memalign_alloc(size_t alignment, size_t bytes, void **result) {
  ASSERT(is_powerof2(alignment) && alignment >= sizeof(void *));
  (void)alignment;
  *result = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  return *result ? MDBX_SUCCESS : MDBX_ENOMEM /* ERROR_OUTOFMEMORY */;
}
#endif /* osal_memalign_alloc */

#ifndef osal_memalign_free
void osal_memalign_free(void *ptr) { VirtualFree(ptr, 0, MEM_RELEASE); }
#endif /* osal_memalign_free */

#ifndef osal_strdup
char *osal_strdup(const char *str) {
  if (!str)
    return nullptr;
  size_t bytes = strlen(str) + 1;
  char *dup = osal_malloc(bytes);
  if (dup)
    memcpy(dup, str, bytes);
  return dup;
}
#endif /* osal_strdup */

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

int osal_condpair_init(osal_condpair_t *condpair) {
  int rc;
  memset(condpair, 0, sizeof(osal_condpair_t));
  if (!(condpair->mutex = CreateMutexW(nullptr, FALSE, nullptr))) {
    rc = (int)GetLastError();
    goto bailout_mutex;
  }
  if (!(condpair->event[0] = CreateEventW(nullptr, FALSE, FALSE, nullptr))) {
    rc = (int)GetLastError();
    goto bailout_event;
  }
  if ((condpair->event[1] = CreateEventW(nullptr, FALSE, FALSE, nullptr)))
    return MDBX_SUCCESS;

  rc = (int)GetLastError();
  (void)CloseHandle(condpair->event[0]);
bailout_event:
  (void)CloseHandle(condpair->mutex);
bailout_mutex:
  memset(condpair, 0, sizeof(osal_condpair_t));
  return rc;
}

int osal_condpair_destroy(osal_condpair_t *condpair) {
  int rc = CloseHandle(condpair->mutex) ? MDBX_SUCCESS : (int)GetLastError();
  rc = CloseHandle(condpair->event[0]) ? rc : (int)GetLastError();
  rc = CloseHandle(condpair->event[1]) ? rc : (int)GetLastError();
  memset(condpair, 0, sizeof(osal_condpair_t));
  return rc;
}

int osal_condpair_lock(osal_condpair_t *condpair) {
  DWORD code = WaitForSingleObject(condpair->mutex, INFINITE);
  return osal_waitstatus2errcode(code);
}

int osal_condpair_unlock(osal_condpair_t *condpair) {
  return ReleaseMutex(condpair->mutex) ? MDBX_SUCCESS : (int)GetLastError();
}

int osal_condpair_signal(osal_condpair_t *condpair, bool part) {
  return SetEvent(condpair->event[part]) ? MDBX_SUCCESS : (int)GetLastError();
}

int osal_condpair_wait(osal_condpair_t *condpair, bool part) {
  DWORD code = SignalObjectAndWait(condpair->mutex, condpair->event[part], INFINITE, FALSE);
  if (code == WAIT_OBJECT_0) {
    code = WaitForSingleObject(condpair->mutex, INFINITE);
    if (code == WAIT_OBJECT_0)
      return MDBX_SUCCESS;
  }
  return osal_waitstatus2errcode(code);
}

/*----------------------------------------------------------------------------*/

int osal_fastmutex_init(osal_fastmutex_t *fastmutex) {
  InitializeCriticalSection(fastmutex);
  return MDBX_SUCCESS;
}

int osal_fastmutex_destroy(osal_fastmutex_t *fastmutex) {
  DeleteCriticalSection(fastmutex);
  return MDBX_SUCCESS;
}

#if MDBX_NATIVE_SEH
#define SEH_TRY(catch)                                                                                                 \
  const size_t _seh_filter = (catch);                                                                                  \
  __try
#define SEH_CATCH                                                                                                      \
  __except ((GetExceptionCode() == _seh_filter ||                                                                      \
             (_seh_filter == EXCEPTION_ACCESS_VIOLATION && GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR))             \
                ? EXCEPTION_EXECUTE_HANDLER                                                                            \
                : EXCEPTION_CONTINUE_SEARCH)
#else
typedef struct seh_simple_context {
  EXCEPTION_REGISTRATION_RECORD Registration;
  size_t filter;
  void *recovery; /* address of recovery/handling/finalization code */
} seh_ctx_t;

EXCEPTION_DISPOSITION NTAPI
#ifdef _Function_class_
_Function_class_(EXCEPTION_ROUTINE)
#endif /* _Function_class_ */
    mdbx_simple_SEH(_Inout_ struct _EXCEPTION_RECORD *pExceptionRecord, _In_ PVOID EstablisherFrame,
                    _Inout_ struct _CONTEXT *pContextRecord, _In_ PVOID DispatcherContext) {
  (void)DispatcherContext;
  (void)EstablisherFrame;
  if (pExceptionRecord->ExceptionCode) {
    NT_TIB *const TIB = (NT_TIB *)NtCurrentTeb();
    seh_ctx_t *seh = (seh_ctx_t *)TIB->ExceptionList;
#ifndef EXCEPTION_CHAIN_END
#define EXCEPTION_CHAIN_END ((PEXCEPTION_REGISTRATION_RECORD) - 1)
#endif
    while (seh && seh != (seh_ctx_t *)EXCEPTION_CHAIN_END) {
      if (seh->Registration.Handler == mdbx_simple_SEH &&
          (seh->filter == pExceptionRecord->ExceptionCode ||
           (seh->filter == EXCEPTION_ACCESS_VIOLATION && pExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR))) {
#ifdef _WIN64
        pContextRecord->Rip = (uintptr_t)seh->recovery;
#else
        pContextRecord->Eip = (uintptr_t)seh->recovery;
#endif
        seh->recovery = 0;
        return ExceptionContinueExecution;
      }
      seh = (seh_ctx_t *)seh->Registration.Next;
    }
  }
  return ExceptionContinueSearch;
}

#if defined(_MSC_VER) && !defined(__clang__)
#define _SEH_SETUP_RECOVERY(CTX, LABEL) __asm { mov [CTX.recovery], offset LABEL }
#else
#define _SEH_SETUP_RECOVERY(CTX, LABEL) CTX.recovery = &&LABEL;
#endif

#define SEH_TRY(catch)                                                                                                 \
  seh_ctx_t _seh_ctx;                                                                                                  \
  NT_TIB *const _TIB = (NT_TIB *)NtCurrentTeb();                                                                       \
  _seh_ctx.Registration.Handler = mdbx_simple_SEH;                                                                     \
  _seh_ctx.filter = (catch);                                                                                           \
  _SEH_SETUP_RECOVERY(_seh_ctx, _seh_recovery)                                                                         \
  _seh_ctx.Registration.Next = _TIB->ExceptionList;                                                                    \
  _TIB->ExceptionList = &_seh_ctx.Registration;

#define SEH_CATCH                                                                                                      \
  _seh_recovery : if (_TIB->ExceptionList == &_seh_ctx.Registration)                                                   \
    _TIB->ExceptionList = _seh_ctx.Registration.Next;                                                                  \
  if (!_seh_ctx.recovery)

#endif /* !MDBX_NATIVE_SEH */

#if !MDBX_NATIVE_SEH
#ifdef __clang__
/* FIXME: TODO */
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4740) /* flow in or out of inline asm code suppresses global optimization */
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#endif /* !MDBX_NATIVE_SEH */
int osal_fastmutex_acquire(osal_fastmutex_t *fastmutex) {
  SEH_TRY(0xC0000194 /* STATUS_POSSIBLE_DEADLOCK / EXCEPTION_POSSIBLE_DEADLOCK */) { EnterCriticalSection(fastmutex); }
  SEH_CATCH { return MDBX_EDEADLK; }
  return MDBX_SUCCESS;
}

bool osal_safe_peek_int32(const void *ptr, int32_t *dest) {
  bool done = false;
  *dest = 0;

  SEH_TRY(EXCEPTION_ACCESS_VIOLATION) {
    if (IsBadReadPtr(ptr, 4) == 0) {
      memcpy(dest, ptr, 4);
      done = true;
    }
  }
  SEH_CATCH { return false; }
  return done;
}

#if !MDBX_NATIVE_SEH
#ifdef __clang__
/* FIXME: TODO */
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif /* !MDBX_NATIVE_SEH */

int osal_fastmutex_release(osal_fastmutex_t *fastmutex) {
  LeaveCriticalSection(fastmutex);
  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

int osal_mb2w(const char *const src, wchar_t **const pdst) {
  const size_t dst_wlen = MultiByteToWideChar(CP_THREAD_ACP, MB_ERR_INVALID_CHARS, src, -1, nullptr, 0);
  wchar_t *dst = *pdst;
  int rc = ERROR_INVALID_NAME;
  if (unlikely(dst_wlen < 2 || dst_wlen > /* MAX_PATH */ INT16_MAX))
    goto bailout;

  dst = osal_realloc(dst, dst_wlen * sizeof(wchar_t));
  rc = MDBX_ENOMEM;
  if (unlikely(!dst))
    goto bailout;

  *pdst = dst;
  if (likely(dst_wlen == (size_t)MultiByteToWideChar(CP_THREAD_ACP, MB_ERR_INVALID_CHARS, src, -1, dst, (int)dst_wlen)))
    return MDBX_SUCCESS;

  rc = ERROR_INVALID_NAME;
bailout:
  if (*pdst) {
    osal_free(*pdst);
    *pdst = nullptr;
  }
  return rc;
}
