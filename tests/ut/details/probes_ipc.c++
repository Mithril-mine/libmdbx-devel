/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026

/* File-IPC round-trip for the probe-bus (mdbx-probes): the test acts as the
 * external driver, writing requests to <dir>/cmd and reading replies from
 * <dir>/rep, while the library drains them lazily on the next mdbx_probe_ctl()
 * call ("sync" is the deterministic barrier). A GoogleTest unit per
 * tests/AGENTS.md §12 (issue #519 stage 6): the IPC directory is created and
 * removed by the test itself (§4 hygiene). */

#include "../test-env.h++"

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif /* _WIN32 */

/*-----------------------------------------------------------------------------*/

namespace {
namespace fs = std::filesystem;

constexpr const char kDir[] = "probes-ipc";
constexpr const char kCtl[] = "MDBX_PROBE_CTL";

/* putenv() keeps the pointer, so the buffer must outlive the call - hence
 * the static storage (the original used a main()'s local for the same
 * reason). */
static char ctl_env[1024];

void set_ctl_env(const char *dir) {
#if defined(_WIN32)
  ASSERT_TRUE(SetEnvironmentVariableA(kCtl, dir));
#else
  snprintf(ctl_env, sizeof(ctl_env), "%s=%s", kCtl, dir);
  ASSERT_EQ(0, putenv(ctl_env));
#endif /* _WIN32 */
}

} /* namespace */

TEST(probes_ipc, file_round_trip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  std::error_code ec;
  fs::remove_all(kDir, ec);
  ASSERT_TRUE(fs::create_directories(kDir));
  const std::string dir = fs::absolute(kDir).string();

  /* The driver must set MDBX_PROBE_CTL before any probe fires.
   * NB: on Windows the engine reads env via GetEnvironmentVariableA (Win32
   * process environment block), while CRT putenv() only updates the CRT table
   * - the two diverge, so use the Win32 API here. */
  set_ctl_env(dir.c_str());

  char path[1024];
  /* Requests: append two commands, each on its own line. */
  snprintf(path, sizeof(path), "%s/cmd", dir.c_str());
  FILE *cmd = fopen(path, "wb");
  ASSERT_NE(nullptr, cmd);
  fputs("mode count\n", cmd);
  fputs("alloc-fault 0\n", cmd);
  fclose(cmd);

  /* Barrier: the library drains <dir>/cmd on this call and appends replies. */
  char reply[256];
  reply[0] = 0;
  ASSERT_EQ(0, mdbx_tst_probe_ctl("sync", reply, sizeof(reply)));
  EXPECT_NE(strstr(reply, "ok"), nullptr);

  /* Wait for the lazy drain to have produced replies. */
  snprintf(path, sizeof(path), "%s/rep", dir.c_str());
  FILE *rep = fopen(path, "rb");
  ASSERT_NE(nullptr, rep);
  char buf[512];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, rep);
  fclose(rep);
  buf[n] = 0;
  EXPECT_NE(strstr(buf, "ok"), nullptr);
  /* Two drained commands -> at least two "ok" lines. */
  size_t oks = 0;
  for (const char *p = buf; (p = strstr(p, "ok\n")) != NULL; p += 3)
    ++oks;
  EXPECT_GE(oks, 2u);

  fs::remove_all(kDir, ec);
}
