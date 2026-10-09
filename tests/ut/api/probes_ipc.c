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
/// \date 2025-2026

/* File-IPC round-trip for the probe-bus (mprobe v2): the test acts as the
 * external driver, writing requests to <dir>/cmd and reading replies from
 * <dir>/rep, while the library drains them lazily on the next mprobe_ctl()
 * call ("sync" is the deterministic barrier). */

#include "src/essentials.h"
#if !defined(MDBX_PROBES)
#error "probes_ipc.c must be built with MDBX_PROBES defined"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      ++failures;                                                                                                      \
      fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #cond);                                                 \
    }                                                                                                                  \
  } while (0)

#define CHECK_REPLY(buf, needle)                                                                                       \
  do {                                                                                                                 \
    if (!strstr((buf), (needle))) {                                                                                    \
      ++failures;                                                                                                      \
      fprintf(stderr, "FAIL %s:%u: reply \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, (buf), (needle));                \
    }                                                                                                                  \
  } while (0)

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <dir>\n", argv[0]);
    return EXIT_FAILURE;
  }
  const char *dir = argv[1];

  /* The driver must set MDBX_PROBE_CTL before any probe fires. */
  char env[1024];
  snprintf(env, sizeof(env), "MDBX_PROBE_CTL=%s", dir);
  putenv(env);

  char path[1024];
  /* Requests: append two commands, each on its own line. */
  snprintf(path, sizeof(path), "%s/cmd", dir);
  FILE *cmd = fopen(path, "wb");
  CHECK(cmd != NULL);
  if (!cmd)
    return EXIT_FAILURE;
  fputs("mode count\n", cmd);
  fputs("alloc-fault 0\n", cmd);
  fclose(cmd);

  /* Barrier: the library drains <dir>/cmd on this call and appends replies. */
  char reply[256];
  reply[0] = 0;
  CHECK(mprobe_ctl("sync", reply, sizeof(reply)) == 0);
  CHECK_REPLY(reply, "ok");

  /* Wait for the lazy drain to have produced replies. */
  snprintf(path, sizeof(path), "%s/rep", dir);
  FILE *rep = fopen(path, "rb");
  CHECK(rep != NULL);
  if (rep) {
    char buf[512];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, rep);
    fclose(rep);
    buf[n] = 0;
    CHECK(strstr(buf, "ok") != NULL);
    /* Two drained commands -> at least two "ok" lines. */
    size_t oks = 0;
    for (const char *p = buf; (p = strstr(p, "ok\n")) != NULL; p += 3)
      ++oks;
    CHECK(oks >= 2);
  }

  if (failures) {
    fprintf(stderr, "probes_ipc: %u FAILURE(S)\n", failures);
    return EXIT_FAILURE;
  }
  fprintf(stderr, "probes_ipc: PASS\n");
  return EXIT_SUCCESS;
}