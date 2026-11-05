/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/361
 *
 * With C11 atomics disabled, the emulated atomic_load*() helpers applied the
 * hardware fence before the read (and the emulated stores after the write),
 * so the acquire/release ordering of the publication pattern was silently
 * lost on weakly-ordered CPUs: a reader could observe a freshly published
 * sequence number together with a stale payload.
 *
 * This test runs the seqlock publication pattern (the odd sequence number
 * marks the update in progress, then the payload, then the even sequence
 * number completes it, all through the emulated atomic helpers) against
 * reader threads which validate that every observed sequence number is
 * paired with its exactly matching payload. */

#include "atomics-ops.h"

#if !defined(__cplusplus) && defined(MDBX_HAVE_C11ATOMICS)
#error "This test must be built without C11 atomics (the emulated fallback path)."
#endif

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PUBLISH_LOOPS (1 << 20)

static mdbx_atomic_uint32_t g_done;
static mdbx_atomic_uint32_t g_seq32;
static mdbx_atomic_uint32_t g_flags32;
static mdbx_atomic_uint64_t g_seq64;
static mdbx_atomic_uint64_t g_payload64;

static uint32_t expected32(uint32_t seq) { return seq * 2654435761u + 12345u; }

static uint64_t expected64(uint64_t seq) { return (seq + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull ^ (seq >> 31); }

static void fail(const char *what, uint64_t seq, uint64_t got, uint64_t want) {
  fprintf(stderr,
          "issue_gh0361_publication: torn publication of %s at seq %" PRIu64 ": got %" PRIu64 ", want %" PRIu64 "\n",
          what, seq, got, want);
  exit(1);
}

static void *writer(void *arg) {
  (void)arg;
  for (uint32_t round = 1; round <= PUBLISH_LOOPS; ++round) {
    const uint32_t odd32 = round * 2 - 1, even32 = round * 2;
    atomic_store32(&g_seq32, odd32, mo_AcquireRelease);
    atomic_store32(&g_flags32, expected32(even32), mo_Relaxed);
    atomic_store32(&g_seq32, even32, mo_AcquireRelease);

    const uint64_t odd64 = odd32, even64 = even32;
    atomic_store64(&g_seq64, odd64, mo_AcquireRelease);
    atomic_store64(&g_payload64, expected64(even64), mo_Relaxed);
    atomic_store64(&g_seq64, even64, mo_AcquireRelease);
  }
  atomic_store32(&g_done, 1, mo_AcquireRelease);
  return nullptr;
}

static void *reader(void *arg) {
  (void)arg;
  uint32_t last_seq32 = 0;
  uint64_t last_seq64 = 0;
  for (;;) {
    /* the seqlock pattern: only an even (completed) sequence number
     * certifies the payload; the acquire ordering of the payload load is
     * essential, otherwise the re-check below may be executed before it */
    const uint32_t seq32 = atomic_load32(&g_seq32, mo_AcquireRelease);
    if ((seq32 & 1) == 0 && seq32 != last_seq32) {
      const uint32_t flags = atomic_load32(&g_flags32, mo_AcquireRelease);
      if (atomic_load32(&g_seq32, mo_AcquireRelease) == seq32) {
        if (unlikely(flags != expected32(seq32)))
          fail("32-bit payload", seq32, flags, expected32(seq32));
        last_seq32 = seq32;
      }
    }
    const uint64_t seq64 = atomic_load64(&g_seq64, mo_AcquireRelease);
    if ((seq64 & 1) == 0 && seq64 != last_seq64) {
      const uint64_t payload = atomic_load64(&g_payload64, mo_AcquireRelease);
      if (atomic_load64(&g_seq64, mo_AcquireRelease) == seq64) {
        if (unlikely(payload != expected64(seq64)))
          fail("64-bit payload", seq64, payload, expected64(seq64));
        last_seq64 = seq64;
      }
    }
    if (atomic_load32(&g_done, mo_AcquireRelease) && seq32 == PUBLISH_LOOPS * 2 && seq64 == PUBLISH_LOOPS * 2)
      return nullptr;
  }
}

int main(void) {
  atomic_store32(&g_done, 0, mo_Relaxed);
  atomic_store32(&g_seq32, 0, mo_Relaxed);
  atomic_store32(&g_flags32, expected32(0), mo_Relaxed);
  atomic_store64(&g_seq64, 0, mo_Relaxed);
  atomic_store64(&g_payload64, expected64(0), mo_Relaxed);

  pthread_t readers[2];
  for (size_t i = 0; i < 2; ++i)
    if (pthread_create(&readers[i], nullptr, reader, nullptr) != 0) {
      fprintf(stderr, "issue_gh0361_publication: pthread_create() failed\n");
      return 2;
    }
  pthread_t writer_thread;
  if (pthread_create(&writer_thread, nullptr, writer, nullptr) != 0) {
    fprintf(stderr, "issue_gh0361_publication: pthread_create() failed\n");
    return 2;
  }

  for (size_t i = 0; i < 2; ++i)
    pthread_join(readers[i], nullptr);
  pthread_join(writer_thread, nullptr);
  printf("issue_gh0361_publication: ok, %u rounds\n", PUBLISH_LOOPS);
  return 0;
}
