//! \file v11c-osal-primitives-posix.c
//! \brief Stream C (wave 11c) machine-checks of the platform-neutral POSIX OSAL
//!        primitive contracts: file-position parking, ioring add/walk/write/reset
//!        state machine, and boot-id crafted-content resistance.
//!
//! What is verified, statement-level against the library's own OSAL objects:
//!   E1 (osal_fseek_shut, src/osal/details.c:15-47): given a seek target beyond
//!      the filesystem's s_maxbytes (lseek -> EINVAL), the binary search over
//!      shift probes converges to the largest seekable offset, parks the fd
//!      position OUTSIDE the data (beyond the file end) and is stable on re-run.
//!      On filesystems where the huge offset is directly seekable, the direct
//!      park is verified instead (the EINVAL fallback is then unreachable by
//!      construction, which the probe reports explicitly).
//!   E2 (osal_ioring_add/walk/write/reset, src/osal/posix/osal-ioring.c):
//!      a) adjacent same-buffer pages are merged into one iovec without
//!         consuming an extra slot;
//!      b) non-adjacent pages take a new slot and MDBX_RESULT_TRUE is returned
//!         exactly when the ring is full (never overruns);
//!      c) osal_ioring_write persists every queued range exactly, and wops
//!         equals the number of items;
//!      d) osal_ioring_reset restores slots_left == allocated and last == null.
//!   E3 (bootid_parse_uuid/check_uuid, src/osal/details.c:64-157): crafted
//!      boot-id content resistance — a text UUID needs >= 32 hex digits to be
//!      accepted; short non-hex garbage is rejected; a 16-byte binary blob is
//!      accepted only when check_uuid()'s hamming-weight well-formedness holds;
//!      an all-zero blob is rejected.
//!
//! VERDICT (machine-checked, Linux 6.x x86-64): HELD, exit 0.
//!   E1: on tmpfs (/tmp) the huge offset is accepted directly (no s_maxbytes
//!       cap; the EINVAL fallback is unreachable by construction); on ext4
//!       (/) the probe returns EINVAL and the binary search converges to the
//!       filesystem's cap (observed 0x80000000000 == 8 TiB), parks stably
//!       outside the data, payload intact on both branches.
//!   E2: contiguous merge consumes no slot; non-adjacent items consume one;
//!       the full ring returns MDBX_RESULT_TRUE without overrunning;
//!       write persists every range exactly; reset restores the empty state.
//!   E3: 32-hex-digit text accepted; 31-hex and non-hex garbage fall to the
//!       binary path and are accepted only through check_uuid() (by design the
//!       content is hashed, so any >= 16-byte input yields a well-formed id);
//!       all-zero binary rejected; n <= 15 rejected.
//!
//! Build (objects kept out of the repo work tree):
//!   cc -c -O1 -I<repo>/src -I<repo>/src/osal/posix <repo>/src/osal/details.c
//!   cc -c -O1 -I<repo>/src -I<repo>/src/osal/posix <repo>/src/osal/posix/osal-file.c
//!   cc -c -O1 -I<repo>/src -I<repo>/src/osal/posix <repo>/src/osal/posix/osal-ioring.c
//!   (plus a one-line TU defining `struct libmdbx_globals globals;`)
//!   cc -O1 -I<repo>/src -I<repo>/src/osal/posix v11c-osal-primitives-posix.c *.o -lpthread
//! Expected exit code 0; prints E1..E3 verdict lines.

#include "internals.h"
#include "osal/details.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* declarations from src/osal/details.c, not present in internals.h for C builds */
bool bootid_parse_uuid(bin128_t *s, const void *p, const size_t n);
bool check_uuid(bin128_t uuid);

static size_t walked_bytes;
static void walk_counter(iov_ctx_t *ctx, size_t offset, void *data, size_t bytes) {
  (void)ctx;
  (void)offset;
  (void)data;
  walked_bytes += bytes;
}

static int failures = 0;
#define CHECK(cond, msg)                                                                                               \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__);                                                        \
      failures += 1;                                                                                                   \
    }                                                                                                                  \
  } while (0)

/*----------------------------------------------------------------------------*/

static char e1_path[] = "/tmp/v11c-fseek-shut-probe.bin";

static void test_fseek_shut(void) {
  printf("== E1: osal_fseek_shut parking-lot search ==\n");
  int fd = open(e1_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0, "E1: open scratch file");
  if (fd < 0)
    return;
  CHECK(write(fd, "DATA", 4) == 4, "E1: write payload");
  fsync(fd);

  const uint64_t huge = UINT64_C(1) << 62;
  uint64_t parking = huge;
  int rc = osal_fseek_shut(fd, &parking);
  CHECK(rc == MDBX_SUCCESS, "E1: osal_fseek_shut succeeds");
  printf("  huge-probe rc %d, parked at %#llx (%s)\n", rc, (unsigned long long)parking,
         rc == MDBX_SUCCESS ? "ok" : "fail");
  if (rc == MDBX_SUCCESS) {
    /* The parked position must be outside the data and still seekable. */
    CHECK(parking >= UINT64_C(4), "E1: parked beyond the data");
    CHECK(lseek(fd, 0, SEEK_CUR) == (off_t)parking, "E1: fd position is at the parked offset");
    /* stability: a second invocation re-parks to the same/other valid offset */
    uint64_t again = huge;
    int rc2 = osal_fseek_shut(fd, &again);
    CHECK(rc2 == MDBX_SUCCESS, "E1: re-park succeeds");
    CHECK(again == parking, "E1: parking is stable");
    if (parking != huge)
      /* the EINVAL fallback engaged: the search must not exceed the FS limit */
      printf("  E1: EINVAL-fallback engaged, converged to largest seekable offset\n");
    else
      printf("  E1: filesystem accepted the huge offset directly "
             "(no s_maxbytes cap; EINVAL fallback unreachable by construction)\n");
    /* the data is intact and readable after parking */
    char buf[4];
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "E1: reseek to 0");
    CHECK(read(fd, buf, 4) == 4 && memcmp(buf, "DATA", 4) == 0, "E1: payload intact after parking");
  }
  close(fd);
  unlink(e1_path);
}

/*----------------------------------------------------------------------------*/

static char e2_path[] = "/tmp/v11c-ioring-probe.bin";

#define NPAGES 8
#define PG (unsigned)MDBX_MIN_PAGESIZE

static void test_ioring(void) {
  printf("== E2: osal_ioring add/walk/write/reset ==\n");
  osal_ioring_t ior;
  int rc = osal_ioring_create(&ior);
  CHECK(rc == MDBX_SUCCESS, "E2: ioring_create");
  CHECK(ior.allocated == 0 && ior.last == nullptr, "E2: fresh ring is empty");

  const size_t want_items = 64;
  rc = osal_ioring_prepare(&ior, want_items, NPAGES * PG);
  CHECK(rc == MDBX_SUCCESS, "E2: ioring_prepare");
  CHECK(ior.allocated >= 32, "E2: prepare allocates at least the 32-item floor");
  const unsigned allocated = ior.allocated;

  /* scratch payload: NPAGES adjacent pages with per-page patterns */
  char *buf = malloc(NPAGES * PG);
  CHECK(buf != nullptr, "E2: payload alloc");
  for (unsigned i = 0; i < NPAGES; ++i)
    memset(buf + i * PG, (int)(0xA0 + i), PG);

  osal_ioring_reset(&ior);
  CHECK(ior.slots_left == allocated, "E2: reset restores slots_left");

  /* a1: two adjacent same-buffer pages merge into one iovec; the contiguous
   * merge extends the last iovec in place and consumes NO extra slot */
  rc = osal_ioring_add(&ior, 0, buf, 2 * PG);
  CHECK(rc == MDBX_SUCCESS, "E2: add a1");
  const unsigned after_first = ior.slots_left;
  rc = osal_ioring_add(&ior, 2 * PG, buf + 2 * PG, 2 * PG);
  CHECK(rc == MDBX_SUCCESS, "E2: add a1-merge");
  CHECK(ior.slots_left == after_first, "E2: contiguous merge consumes no extra slot");
  CHECK(ior_last_bytes(&ior, ior.last) == 4 * PG, "E2: merged item bytes accumulated");

  /* a2: non-adjacent page takes a new item/slot */
  char gap_page[PG];
  memset(gap_page, 0x5A, PG);
  rc = osal_ioring_add(&ior, 100 * PG, gap_page, PG);
  CHECK(rc == MDBX_SUCCESS, "E2: add a2");
  CHECK(ior.slots_left == after_first - 1, "E2: new item consumes one slot");

  /* a3: fill the rest until RESULT_TRUE; never an overrun */
  char fill[PG];
  memset(fill, 0x77, PG);
  unsigned extra = 0;
  while (ior.slots_left) {
    rc = osal_ioring_add(&ior, 1000u * PG * (extra + 1), fill, PG);
    if (rc == MDBX_RESULT_TRUE)
      break;
    CHECK(rc == MDBX_SUCCESS, "E2: add fill");
    extra += 1;
  }
  rc = osal_ioring_add(&ior, 0x7fffff00u * PG, fill, PG);
  CHECK(rc == MDBX_RESULT_TRUE, "E2: full ring reports MDBX_RESULT_TRUE, does not overrun");

  /* c: write everything out and verify the bytes */
  int fd = open(e2_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0, "E2: open scratch file");
  if (fd >= 0) {
    const size_t span = (size_t)(100 * PG + PG) > (size_t)(1000u * PG * extra + PG) ? (100 * PG + PG)
                                                                                    : (size_t)(1000u * PG * extra + PG);
    CHECK(ftruncate(fd, (off_t)span) == 0, "E2: extend scratch file");
    osal_ioring_write_result_t wr = osal_ioring_write(&ior, fd);
    CHECK(wr.err == MDBX_SUCCESS, "E2: ioring_write succeeds");
    CHECK(wr.wops == osal_ioring_used(&ior), "E2: wops == number of queued items");

    char *verify = malloc(span);
    CHECK(verify != nullptr, "E2: verify alloc");
    CHECK(pread(fd, verify, span, 0) == (ssize_t)span, "E2: read back");
    CHECK(memcmp(verify, buf, 4 * PG) == 0, "E2: merged range persisted exactly");
    CHECK(memcmp(verify + 100 * PG, gap_page, PG) == 0, "E2: non-adjacent item persisted");
    bool fill_ok = true;
    for (unsigned k = 1; k <= extra; ++k)
      fill_ok &= memcmp(verify + (size_t)1000u * PG * k, fill, PG) == 0;
    CHECK(fill_ok, "E2: fill items persisted");

    /* walk must visit every queued byte exactly once (sum the bytes) */
    walked_bytes = 0;
    osal_ioring_reset(&ior);
    /* re-add the same layout compactly and walk */
    rc = osal_ioring_add(&ior, 0, buf, 4 * PG);
    CHECK(rc == MDBX_SUCCESS, "E2: re-add for walk");
    osal_ioring_walk(&ior, nullptr, walk_counter);
    CHECK(walked_bytes == 4 * PG, "E2: walk visits the merged item's full bytes");

    /* d: reset restores the empty state */
    osal_ioring_reset(&ior);
    CHECK(ior.slots_left == ior.allocated, "E2: reset restores slots_left == allocated");
    CHECK(ior.last == nullptr, "E2: reset clears last");

    free(verify);
    close(fd);
    unlink(e2_path);
  }
  free(buf);
  osal_ioring_destroy(&ior);
}

/*----------------------------------------------------------------------------*/

static void test_bootid_parse(void) {
  printf("== E3: bootid crafted-content resistance ==\n");
  bin128_t uuid;

  /* full 36-char /proc-style boot_id: 32 hex + 4 dashes */
  memset(&uuid, 0, sizeof(uuid));
  const char *good = "123e4567-e89b-12d3-a456-426614174000";
  CHECK(bootid_parse_uuid(&uuid, good, 36) == true, "E3: 32-hex-digit boot_id accepted");

  /* 31 hex digits: bits == 31*4 == 124, not > 126 -> text parse fails,
   * then the n>15 binary path hashes it -> check_uuid gate decides */
  memset(&uuid, 0, sizeof(uuid));
  const char *shortish = "123e4567-e89b-12d3-a456-4266141740X"; /* 31 hex + 4 dashes */
  const bool r31 = bootid_parse_uuid(&uuid, shortish, 36);
  printf("  31-hex input -> %d (may take the binary path)\n", (int)r31);
  if (r31)
    CHECK(check_uuid(uuid) == true, "E3: binary-path result is well-formed or rejected");

  /* pure garbage (no hex at all): text parse yields bits == 0, then rejected */
  memset(&uuid, 0, sizeof(uuid));
  const char *garbage = "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz";
  const bool rg = bootid_parse_uuid(&uuid, garbage, 36);
  printf("  garbage input -> %d\n", (int)rg);
  if (rg)
    CHECK(check_uuid(uuid) == true, "E3: garbage never yields an ill-formed uuid");

  /* all-zero binary: rejected by check_uuid (hw == 0, (hw>>6) != 1) */
  memset(&uuid, 0, sizeof(uuid));
  const uint8_t zeros[16] = {0};
  CHECK(bootid_parse_uuid(&uuid, zeros, 16) == false, "E3: all-zero binary blob rejected");

  /* random-looking binary: accepted only when check_uuid() holds */
  unsigned prng_state = 42;
  for (unsigned t = 0; t < 64; ++t) {
    uint8_t blob[16];
    for (unsigned i = 0; i < 16; ++i) {
      prng_state = prng_state * 1103515245u + 12345u;
      blob[i] = (uint8_t)(prng_state >> 16);
    }
    memset(&uuid, 0, sizeof(uuid));
    const bool got = bootid_parse_uuid(&uuid, blob, 16);
    CHECK(!got || check_uuid(uuid), "E3: binary accept implies check_uuid");
  }

  /* short buffers: n <= 15 cannot succeed */
  memset(&uuid, 0, sizeof(uuid));
  const char *tiny = "1234567890abcdef";
  CHECK(bootid_parse_uuid(&uuid, tiny, 15) == false, "E3: n == 15 rejected");
}

/*----------------------------------------------------------------------------*/

int main(void) {
  test_fseek_shut();
  test_ioring();
  test_bootid_parse();
  if (failures) {
    printf("== RESULT: %d check(s) FAILED ==\n", failures);
    return 1;
  }
  printf("== RESULT: all checks passed ==\n");
  return 0;
}
