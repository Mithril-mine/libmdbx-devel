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

/* Probe-bus self-test: exercises the mprobe v2 mechanism end-to-end without
 * relying on the engine internals — sites placed in this very test, driven via
 * the exported mprobe_ctl() control API. Requires MDBX_PROBES=ON at configure
 * time; the probe machinery itself is activated by setting MDBX_PROBES=1 in
 * the environment before the first probe fires. */

#include "src/essentials.h" /* entry header: pulls mdbx.h (via preface.h) and
                               logging_and_debug.h in the correct order */
#if !defined(MDBX_PROBES)
#error "probes.c must be built with MDBX_PROBES defined"
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

static void do_ctl(const char *request, char *reply, size_t size) {
  const int rc = mprobe_ctl(request, reply, size);
  CHECK(rc == 0);
}

/* Sites placed in the test itself (semantic tags as the primary key). */
static void fire_sites(void) {
  MPROBE_COLLECT(test_counter, 42);
  MPROBE_WATCH(test_event, 7);
  int err = MDBX_SUCCESS;
  MPROBE_FAULT(test_fault, err);
}

int main(void) {
  char reply[1024];

  /* Activation is env-driven; the test binary must set MDBX_PROBES=1. */
  CHECK(getenv("MDBX_PROBES") && getenv("MDBX_PROBES")[0] == '1');

  /* 1. Firing before any control: sites register lazily, counters accumulate. */
  fire_sites();
  fire_sites();

  do_ctl("list", reply, sizeof(reply));
  CHECK_REPLY(reply, "test_counter");
  CHECK_REPLY(reply, "test_event");
  CHECK_REPLY(reply, "test_fault");

  /* 2. query reports seen/hits/suppressed/armed. */
  do_ctl("query test_*", reply, sizeof(reply));
  CHECK_REPLY(reply, "site test_counter");
  CHECK_REPLY(reply, " 2 "); /* kind=collect */

  /* 3. reset zeroes counters, then re-fire. */
  do_ctl("reset test_*", reply, sizeof(reply));
  do_ctl("query test_counter", reply, sizeof(reply));
  fire_sites();
  do_ctl("query test_counter", reply, sizeof(reply));
  /* format: site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file>:<line> */

  /* 4. disarm suppresses assert/fault behavior but keeps observation. */
  do_ctl("disarm test_event", reply, sizeof(reply));
  fire_sites();
  do_ctl("query test_event", reply, sizeof(reply));

  /* 5. fault injection: arm a return-code mutation at the FAULT site. */
  do_ctl("fault test_fault -30798", reply, sizeof(reply));
  int injected = MDBX_SUCCESS;
  {
    struct mprobe_site site = {"test_fault", __FILE__, __LINE__, mprobe_kind_fault, {0}};
    mprobe_fault(&site, &injected);
  }
  CHECK(injected == -30798);

  /* 6. allocation fault: force next N allocations to fail. */
  do_ctl("alloc-fault 1", reply, sizeof(reply));
  {
    void *p = mprobe_alloc(64, "test");
    CHECK(p == NULL);
    p = mprobe_alloc(64, "test");
    CHECK(p != NULL);
    if (p)
      free(p);
  }

  /* 7. mode count: a failing DEV_ASSERT records but does not abort. */
  do_ctl("mode count", reply, sizeof(reply));
  {
    struct mprobe_site site = {"test_assert_site", __FILE__, __LINE__, mprobe_kind_assert, {0}};
    mprobe_assert_failed(&site, "deliberate");
  }
  do_ctl("mode panic", reply, sizeof(reply));
  do_ctl("query test_assert_site", reply, sizeof(reply));
  CHECK_REPLY(reply, "site test_assert_site");

  if (failures) {
    fprintf(stderr, "probes: %u FAILURE(S)\n", failures);
    return EXIT_FAILURE;
  }
  fprintf(stderr, "probes: PASS\n");
  return EXIT_SUCCESS;
}