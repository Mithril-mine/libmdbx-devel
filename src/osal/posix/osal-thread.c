/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-thread.c (POSIX part) — condition pairs, fast mutexes and
/// threads.

#include "../../internals.h"
#include "../posix/private.h"
#include "../details.h"

static const char dev_null[] = "/dev/null";

#ifndef osal_memalign_alloc
int osal_memalign_alloc(size_t alignment, size_t bytes, void **result) {
  ASSERT(is_powerof2(alignment) && alignment >= sizeof(void *));
#if defined(_ISOC11_SOURCE)
  *result = aligned_alloc(alignment, ceil_powerof2(bytes, alignment));
  return *result ? MDBX_SUCCESS : errno;
#elif _POSIX_VERSION >= 200112L && (!defined(__ANDROID_API__) || __ANDROID_API__ >= 17)
  *result = nullptr;
  return posix_memalign(result, alignment, bytes);
#elif __GLIBC_PREREQ(2, 16) || __STDC_VERSION__ >= 201112L
  *result = memalign(alignment, bytes);
  return *result ? MDBX_SUCCESS : errno;
#else
#error FIXME
#endif
}
#endif /* osal_memalign_alloc */

#ifndef osal_memalign_free
void osal_memalign_free(void *ptr) { osal_free(ptr); }
#endif /* osal_memalign_free */

#ifndef osal_strdup

char *osal_strdup(const char *str) {
  if (!str)
    return nullptr;
  const size_t bytes = strlen(str) + 1;
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
  rc = pthread_mutex_init(&condpair->mutex, nullptr);
  if (unlikely(rc != 0))
    goto bailout_mutex;
  rc = pthread_cond_init(&condpair->cond[0], nullptr);
  if (unlikely(rc != 0))
    goto bailout_cond;
  rc = pthread_cond_init(&condpair->cond[1], nullptr);
  if (likely(rc == 0))
    return MDBX_SUCCESS;

  (void)pthread_cond_destroy(&condpair->cond[0]);
bailout_cond:
  (void)pthread_mutex_destroy(&condpair->mutex);
bailout_mutex:
  memset(condpair, 0, sizeof(osal_condpair_t));
  return rc;
}

int osal_condpair_destroy(osal_condpair_t *condpair) {
  int err, rc = pthread_mutex_destroy(&condpair->mutex);
  rc = (err = pthread_cond_destroy(&condpair->cond[0])) ? err : rc;
  rc = (err = pthread_cond_destroy(&condpair->cond[1])) ? err : rc;
  memset(condpair, 0, sizeof(osal_condpair_t));
  return rc;
}

int osal_condpair_lock(osal_condpair_t *condpair) { return osal_pthread_mutex_lock(&condpair->mutex); }

int osal_condpair_unlock(osal_condpair_t *condpair) { return pthread_mutex_unlock(&condpair->mutex); }

int osal_condpair_signal(osal_condpair_t *condpair, bool part) { return pthread_cond_signal(&condpair->cond[part]); }

int osal_condpair_wait(osal_condpair_t *condpair, bool part) {
  return pthread_cond_wait(&condpair->cond[part], &condpair->mutex);
}

/*----------------------------------------------------------------------------*/

int osal_fastmutex_init(osal_fastmutex_t *fastmutex) {
#if MDBX_CHECKING > 1
  pthread_mutexattr_t ma;
  int rc = pthread_mutexattr_init(&ma);
  if (likely(!rc)) {
    rc = pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    if (likely(!rc) || rc == ENOTSUP)
      rc = pthread_mutex_init(fastmutex, &ma);
    pthread_mutexattr_destroy(&ma);
  }
  return rc;
#else
  return pthread_mutex_init(fastmutex, nullptr);
#endif
}

int osal_fastmutex_destroy(osal_fastmutex_t *fastmutex) { return pthread_mutex_destroy(fastmutex); }

int osal_fastmutex_acquire(osal_fastmutex_t *fastmutex) { return osal_pthread_mutex_lock(fastmutex); }

bool osal_safe_peek_int32(const void *ptr, int32_t *dest) {
  bool done = false;
  *dest = 0;

  static volatile int nullfd = -1;
  int fd = nullfd; /* we sure int read is atomic */
  if (fd < 0) {
    static pthread_mutex_t nullfd_mutex = PTHREAD_MUTEX_INITIALIZER;
    int rc = pthread_mutex_lock(&nullfd_mutex);
    if (unlikely(rc != 0)) {
      ERROR("pthread_mutex_lock(nullfd_mutex), err %d", rc);
      return false;
    }
    fd = nullfd;
    if (fd < 0) {
      fd = open(dev_null, O_WRONLY
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif /* O_CLOEXEC */
      );
      if (unlikely(fd < 0)) {
        rc = pthread_mutex_unlock(&nullfd_mutex);
        ERROR("unable open(%s), err %d", dev_null, errno);
        if (unlikely(rc != 0))
          ERROR("pthread_mutex_unlock(nullfd_mutex), err %d", rc);
        return false;
      }
      nullfd = fd; /* we sure int write is atomic */
    }
    rc = pthread_mutex_unlock(&nullfd_mutex);
    if (unlikely(rc != 0))
      ERROR("pthread_mutex_unlock(nullfd_mutex), err %d", rc);
  }
  if (write(fd, ptr, 4) == 4) {
    memcpy(dest, ptr, 4);
    done = true;
  }
  return done;
}

int osal_fastmutex_release(osal_fastmutex_t *fastmutex) { return pthread_mutex_unlock(fastmutex); }

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

int osal_thread_create(osal_thread_t *thread, THREAD_RESULT(THREAD_CALL *start_routine)(void *), void *arg) {
  return pthread_create(thread, nullptr, start_routine, arg);
}

int osal_thread_join(osal_thread_t thread) {
  void *unused_retval = &unused_retval;
  return pthread_join(thread, &unused_retval);
}
