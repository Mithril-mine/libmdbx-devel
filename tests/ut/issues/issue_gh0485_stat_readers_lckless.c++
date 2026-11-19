/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "../env-guard.h++"
#include "mdbx.h"
#include "tool_runner.h"
#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

/* Regression for the defect stat-readers-lckless-exit-codes of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * mdbx_stat -r exited FAILURE when mdbx_reader_list() returned
 * MDBX_RESULT_TRUE ("Reader Table is absent"): the raw RESULT_TRUE flowed
 * into `rc ? EXIT_FAILURE : EXIT_SUCCESS`.
 *
 * The "absent" outcome requires a LCKLESS environment, which needs the
 * filesystem check to answer "read-only" - a real ST_RDONLY mount, since
 * statvfs() cannot be fooled by permissions. The test injects exactly the
 * read-only-mount behavior through the probe bus (no mounts, no chmod,
 * root-proof): the real mdbx_stat binary is run with the probe file-IPC
 * carrying two fault rules,
 *
 *   fault lck_setup_eacces 13  - the lck-file open fails with EACCES;
 *   fault fs_rdonly_flag 1     - the statvfs() answer is injected as
 *                                ST_RDONLY (the ro-mount behavior),
 *
 * so the environment opens in without-lck mode and the reader table is
 * reported absent. With the fix the tool exits 0 (was: exit 1).
 *
 * Probe-build and POSIX only (the fault sites live in the POSIX osal). */

static const std::string stat_tool = tool_runner::tool_path("mdbx_stat");

static std::string slurp(const char *path) {
  std::string out;
  FILE *f = fopen(path, "rb");
  if (!f)
    return out;
  char chunk[512];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    out.append(chunk, n);
  fclose(f);
  return out;
}

TEST(issue_gh0485, stat_readers_absent_is_not_a_failure) {
  const env_files_guard guard({"issue485_lckless.mdbx"});
  const char *const dbname = "issue485_lckless.mdbx";
  const char *const ipcdir = "issue485_probe_ipc";
  const char *const outfile = "issue485_stat.out";

  /* a small database to look at (a NOSUBDIR single file, as mdbx_stat -n
   * expects) */
  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR, 0644));
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &dbi));
    const char key_data[] = "k", value_data[] = "v";
    MDBX_val key = {const_cast<char *>(key_data), 1};
    MDBX_val value = {const_cast<char *>(value_data), 1};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    mdbx_env_close(env);
  }

  /* the probe-bus file IPC with the two fault rules */
  ASSERT_EQ(0, mkdir(ipcdir, 0755)) << "mkdir(" << ipcdir << ")";
  {
    std::string cmd_path = std::string(ipcdir) + "/cmd";
    FILE *f = fopen(cmd_path.c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_LT(0, fputs("arm lck_setup_eacces\n"
                       "fault lck_setup_eacces 13\n"
                       "arm fs_rdonly_flag\n"
                       "fault fs_rdonly_flag 1\n",
                       f));
    ASSERT_EQ(0, fclose(f));
  }

  /* the tool is a child process: it activates the bus and drains the cmd
   * file at the first probe firing, both before the lck setup completes */
  ASSERT_EQ(0, setenv("MDBX_PROBES", "1", 1));
  ASSERT_EQ(0, setenv("MDBX_PROBE_CTL", ipcdir, 1));
  const int rc = tool_runner::run({stat_tool, "-r", "-q", "-n", dbname}, nullptr, outfile, outfile);
  ASSERT_EQ(0, unsetenv("MDBX_PROBES"));
  ASSERT_EQ(0, unsetenv("MDBX_PROBE_CTL"));

  const std::string out = slurp(outfile);
  remove(outfile);
  remove((std::string(ipcdir) + "/cmd").c_str());
  remove((std::string(ipcdir) + "/rep").c_str());
  rmdir(ipcdir);

  /* the scenario must actually engage: the without-lck fallback happened
   * and the reader table was reported absent */
  ASSERT_NE(std::string::npos, out.find("Reader Table is absent"))
      << "the lckless fixture did not engage; stat rc=" << rc << ", output:\n"
      << out;

  /* the defect: the informational outcome leaked into the exit code */
  EXPECT_EQ(0, rc) << "`Reader Table is absent` is informational, not a failure";
}
