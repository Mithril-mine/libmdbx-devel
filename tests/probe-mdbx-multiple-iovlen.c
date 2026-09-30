/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

/* REGRESSION REPRODUCER (standalone diagnostic, not wired into CTest).
 *
 * Plain-C reproducer of the fixed MDBX_MULTIPLE batch-insert defect:
 * when mdbx_put(MDBX_NOOVERWRITE | MDBX_MULTIPLE) hits an already existing
 * key it returns MDBX_KEYEXIST and MUST report zero written items in
 * `data[1].iov_len`.
 *
 * Fixed by initializing the MULTIPLE batch bookkeeping before ALL early-exit
 * paths of cursor_put() (the previous `return MDBX_KEYEXIST` on a duplicate
 * key preceded `*batch_dupfix_done = 0`, leaving the input count behind).
 *
 * Expected: rc == MDBX_KEYEXIST and done == 0.
 * Regression: run with the fixed tree -> both hold; the buggy tree reports
 * done == input count (e.g. 3).
 *
 * Build:  gcc -I. tests/probe-mdbx-multiple-iovlen.c -o /tmp/probe \
 *         -L<build> -lmdbx-static -lpthread
 */

#include "mdbx.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(rc)                                                                                                      \
  do {                                                                                                                 \
    if ((rc) != MDBX_SUCCESS) {                                                                                        \
      fprintf(stderr, "FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, #rc, mdbx_strerror(rc));                           \
      return (rc);                                                                                                     \
    }                                                                                                                  \
  } while (0)

static int step(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const uint32_t *data, size_t count,
                MDBX_put_flags_t flags) {
  MDBX_val k = {(void *)key, strlen(key)};
  MDBX_val args[2] = {{(void *)data, sizeof(uint32_t)}, {NULL, count}};
  int rc = mdbx_put(txn, dbi, &k, args, flags | MDBX_MULTIPLE);
  printf("put %-3s {", key);
  for (size_t i = 0; i < count; i++)
    printf("%s%" PRIu32, i ? "," : "", data[i]);
  printf("} flags=0x%04x -> rc=%s done=%zu\n", (unsigned)flags, mdbx_strerror(rc), (size_t)args[1].iov_len);
  return rc;
}

int main(void) {
  const char *path = "probe-mdbx-multiple-iovlen";
  remove(path);
  MDBX_env *env;
  CHECK(mdbx_env_create(&env));
  CHECK(mdbx_env_set_maxdbs(env, 8));
  CHECK(mdbx_env_open(env, path, MDBX_CREATE | MDBX_NOSUBDIR, 0664));
  MDBX_txn *txn;
  CHECK(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  CHECK(mdbx_dbi_open(txn, "table", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dbi));

  const uint32_t base[4] = {11, 12, 13, 14};
  const uint32_t extra[3] = {15, 16, 17};

  CHECK(step(txn, dbi, "k1", base, 4, MDBX_NOOVERWRITE));
  CHECK(step(txn, dbi, "k2", base, 4, MDBX_UPSERT));
  CHECK(step(txn, dbi, "k2", base + 3, 1, MDBX_CURRENT));

  /* k1 already exists: NOOVERWRITE|MULTIPLE must be KEYEXIST with done==0 */
  int rc = step(txn, dbi, "k1", extra, 3, MDBX_NOOVERWRITE);
  if (rc != MDBX_KEYEXIST) {
    fprintf(stderr, "unexpected rc: %s\n", mdbx_strerror(rc));
    return 1;
  }
  return (0);
}