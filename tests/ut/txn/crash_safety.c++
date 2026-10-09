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

/// Crash-safety coverage without mdbx_test (B13): a writer process killed
/// abnormally mid-write-transaction must leave the environment recoverable,
/// with exactly the committed state and no leaked uncommitted rows.
///
/// POSIX: fork() + raise(SIGKILL). Windows: re-exec of the same gtest binary
/// into the disabled ut_crash_safety.crashed_child test, which dies via
/// TerminateProcess().

#include "mdbx.h++"
#include <gtest/gtest.h>
#include MDBX_CONFIG_H

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr const char *table_name = "data";
constexpr unsigned baseline_count = 5000;
constexpr unsigned child_count = 3000;

static long current_pid() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<long>(getpid());
#endif
}

static std::string key_of(unsigned i, char tag) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%c:%05u", tag, i);
  return buf;
}

static std::string val_of(unsigned i, char tag) {
  const size_t n = tag == 'A' ? 512 : 2048;
  std::string s(n, tag);
  char tail[32];
  snprintf(tail, sizeof(tail), ":%05u:%08x", i, i * 2654435761u);
  s.replace(s.size() - strlen(tail), strlen(tail), tail);
  return s;
}

static mdbx::create_parameters make_create_parameters() {
  mdbx::create_parameters params;
  /* The crash workload dirties several MiB on top of the committed baseline;
   * the platform default geometry upper bound is too small on some 32-bit
   * Windows cells (observed MDBX_MAP_FULL mid-transaction — the same anomaly
   * that forced explicit geometry in dupfix_multiple). Pin a generous dynamic
   * geometry so the test never depends on platform-dependent default sizing. */
  params.geometry.make_dynamic(16 * mdbx::env::geometry::MiB, 256 * mdbx::env::geometry::MiB);
  return params;
}

static void write_baseline(const std::string &path) {
  mdbx::env_managed env(path, make_create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  auto txn = env.start_write();
  auto table = txn.create_map(table_name);
  for (unsigned i = 0; i < baseline_count; ++i)
    txn.upsert(table, mdbx::slice(key_of(i, 'A')), mdbx::slice(val_of(i, 'A')));
  txn.commit();
}

/// Open the environment read-write, start a write transaction and dirty many
/// pages (page splits), then die WITHOUT committing. The committed baseline
/// must survive untouched.
static void crash_child_workload(const std::string &path) {
  mdbx::env_managed env(path, make_create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  auto txn = env.start_write();
  auto table = txn.open_map(table_name);
  for (unsigned i = 0; i < child_count; ++i)
    txn.upsert(table, mdbx::slice(key_of(i, 'C')), mdbx::slice(val_of(i, 'C')));
}

#if !defined(_WIN32)

static bool spawn_crash_child(const std::string &path) {
  pid_t pid = fork();
  if (pid < 0) {
    ADD_FAILURE() << "fork() failed: " << errno;
    return false;
  }
  if (pid == 0) {
    try {
      crash_child_workload(path);
    } catch (...) {
      _exit(126);
    }
    raise(SIGKILL);
    _exit(127); /* unreachable */
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    ADD_FAILURE() << "waitpid() failed: " << errno;
    return false;
  }
  EXPECT_TRUE(WIFSIGNALED(status)) << "crash child must die by signal";
  if (WIFSIGNALED(status)) {
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }
  return true;
}

#else /* _WIN32 */

static bool spawn_crash_child(const std::string &path) {
  char exe[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    ADD_FAILURE() << "GetModuleFileNameA() failed: " << GetLastError();
    return false;
  }

  std::string cmdline = "\"" + std::string(exe) +
                        "\" --gtest_filter=*crashed_child* --gtest_also_run_disabled_tests";
  if (!SetEnvironmentVariableA("MDBX_CRASH_CHILD_ENV", path.c_str())) {
    ADD_FAILURE() << "SetEnvironmentVariableA() failed: " << GetLastError();
    return false;
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessA(exe, &cmdline[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  SetEnvironmentVariableA("MDBX_CRASH_CHILD_ENV", nullptr);
  if (!ok) {
    ADD_FAILURE() << "CreateProcessA() failed: " << GetLastError();
    return false;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  if (!GetExitCodeProcess(pi.hProcess, &code)) {
    ADD_FAILURE() << "GetExitCodeProcess() failed: " << GetLastError();
  } else {
    EXPECT_EQ(code, 42u) << "crash child must die via TerminateProcess(42)";
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

#endif /* _WIN32 */

/// Best-effort mdbx_chk of the crashed environment, mirroring the CI
/// smoke_fault_chk step. The tool is searched relative to the test binary;
/// if it is not found the API-level recovery checks above remain the verdict.
static void run_mdbx_chk_best_effort(const std::string &path) {
#if defined(_WIN32)
  const char *names[] = {"mdbx_chk.exe", "..\\mdbx_chk.exe"};
#else
  const char *names[] = {"mdbx_chk", "../mdbx_chk"};
#endif
  for (const char *rel : names) {
#if defined(_WIN32)
    if (_access(rel, 0) != 0)
      continue;
#else
    if (access(rel, X_OK) != 0)
      continue;
#endif
    /* /bin/sh does not search the current directory: use an explicit "./"
     * prefix on POSIX. cmd.exe searches the CWD itself and rejects "./"
     * ("'.' is not recognized"), so keep the command bare on Windows. */
#if defined(_WIN32)
    const std::string cmd = std::string(rel) + " -vvnw \"" + path + "\"";
#else
    const std::string cmd = "./" + std::string(rel) + " -vvnw \"" + path + "\"";
#endif
    const int rc = system(cmd.c_str());
    if (rc != 0)
      ADD_FAILURE() << "mdbx_chk reported problems (rc=" << rc << ") on the crashed environment";
    return;
  }
  /* mdbx_chk not found; coverage is provided by the recovery checks above */
}

TEST(ut_crash_safety, kill_mid_txn_recovers) {
  const std::string path = "test-crash-safety-" + std::to_string(current_pid());
  mdbx::env::remove(path);

  write_baseline(path);
  ASSERT_TRUE(spawn_crash_child(path));

  // Reopen: recovery must discard the child's uncommitted writes.
  {
    mdbx::env_managed env(path, make_create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto rtxn = env.start_read();
    auto table = rtxn.open_map(table_name);

    for (unsigned i = 0; i < baseline_count; ++i) {
      const std::string k = key_of(i, 'A');
      const std::string expect = val_of(i, 'A');
      const mdbx::slice got = rtxn.get(table, mdbx::slice(k));
      EXPECT_EQ(got.size(), expect.size()) << "value size mismatch at key " << k;
      if (got.size() == expect.size()) {
        EXPECT_EQ(memcmp(got.data(), expect.data(), expect.size()), 0) << "value mismatch at key " << k;
      }
    }

    // The child's uncommitted rows must be absent.
    for (unsigned i = 0; i < child_count; ++i) {
      const std::string k = key_of(i, 'C');
      EXPECT_THROW(rtxn.get(table, mdbx::slice(k)), mdbx::not_found)
          << "uncommitted row leaked at key " << k;
    }
  }

  // The environment must remain fully read-write usable after recovery.
  {
    mdbx::env_managed env(path, make_create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto wtxn = env.start_write();
    auto table = wtxn.open_map(table_name);
    wtxn.upsert(table, mdbx::slice("post-crash"), mdbx::slice("value"));
    wtxn.commit();
  }

  run_mdbx_chk_best_effort(path);

  mdbx::env::remove(path);
}

#if defined(_WIN32)
/// MSVC (/W4 /WX) and clang-cl (-Werror) promote the CRT C4996 'getenv is
/// unsafe' deprecation to a hard error; wrap the lookup like get_cached.c++
/// and bunches_removal.c++ do (see f383ac96).
static const char *getenv_cstr(const char *name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  const char *value = std::getenv(name);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value;
}

TEST(ut_crash_safety, DISABLED_crashed_child) {
  const char *path = getenv_cstr("MDBX_CRASH_CHILD_ENV");
  if (!path) {
    GTEST_SKIP() << "not spawned as crash child";
    return;
  }
  crash_child_workload(path);
  TerminateProcess(GetCurrentProcess(), 42);
  FAIL() << "unreachable";
}
#endif /* _WIN32 */

} // namespace