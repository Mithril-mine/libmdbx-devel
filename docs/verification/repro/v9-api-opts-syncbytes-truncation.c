/* V9 (stream A, api-opts.c) — MDBX_opt_sync_bytes truncates to uint32 without validation.
 *
 * mdbx_env_set_option() case MDBX_opt_sync_bytes (api-opts.c:209-228):
 *   - the only bound is `value > SIZE_MAX - 65536` (bytes, pre-rounding);
 *   - then `value = bytes2pgno(env, value + ps - 1)` (size_t, up to 2^58 pages
 *     for a 64-bit input), and the result is stored as `(uint32_t)value`
 *     into lck->autosync_threshold with NO range check.
 * Consequences (all release-mode, no UB required):
 *   1. mdbx_env_set_option(MDBX_opt_sync_bytes, X) followed by
 *      mdbx_env_get_option() returns pgno2bytes((uint32_t)(X >> ps2ln)) —
 *      i.e. the get/set round-trip diverges for any X >= 2^(32+ps2ln);
 *   2. the effective autosync threshold is the truncated page count, so the
 *      library fires fsync at a threshold radically different from the one
 *      the user asked for (here: asked >= 2^44 bytes, got ~2^44 -> actually
 *      the low 32 bits of the page count).
 *
 * Expected (correct) behavior: EINVAL (or a clamping diagnostic) for values
 * whose page count exceeds UINT32_MAX; get() returns the effective value.
 *
 * Build: gcc -I<src> v9-api-opts-syncbytes-truncation.c -L/tmp/v9-build -lmdbx-static -lpthread
 * Run in a temp directory; creates test DB files testdb.dxb/.lck (cleanup by mdbx_env_delete).
 */
#include "mdbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  system("rm -rf v9sync.tmp && mkdir v9sync.tmp && chdir .");
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS) {
    printf("FAIL env_create rc=%d\n", rc);
    return 1;
  }
  rc = mdbx_env_open(env, "v9sync.tmp", MDBX_WRITEMAP | MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    printf("FAIL env_open rc=%d\n", rc);
    return 1;
  }

  /* 2^44 bytes: page count = 2^32 with 4KiB pages — overflows uint32 storage
   * of autosync_threshold. Deliberately below SIZE_MAX-65536 (the only check). */
  const uint64_t requested = UINT64_C(1) << 44;
  rc = mdbx_env_set_option(env, MDBX_opt_sync_bytes, requested);
  printf("set_option(sync_bytes, 0x%016llx) rc=%d\n", (unsigned long long)requested, rc);
  if (rc != MDBX_SUCCESS) {
    printf("PASS-DIFFERENT: set_option rejected the value (rc=%d) — truncation absent in this build\n", rc);
  } else {
    uint64_t got = 0;
    rc = mdbx_env_get_option(env, MDBX_opt_sync_bytes, &got);
    printf("get_option(sync_bytes) rc=%d value=0x%016llx (%llu)\n", rc, (unsigned long long)got,
           (unsigned long long)got);
    /* page-rounding alone would keep the top bits; truncation loses them */
    if (got >= requested - (1ull << 12) && got <= requested + (1ull << 12))
      printf("PASS: round-trip consistent\n");
    else {
      printf("FAIL (V9-F1 CONFIRMED): set 0x%llx, get 0x%llx — truncated to low 32 bits of the page count\n",
             (unsigned long long)requested, (unsigned long long)got);
      printf("effective autosync threshold is %llu bytes instead of %llu bytes\n", (unsigned long long)got,
             (unsigned long long)requested);
    }
  }

  mdbx_env_close_ex(env, true);
  mdbx_env_delete("v9sync.tmp", MDBX_ENV_JUST_DELETE);
  return 0;
}
