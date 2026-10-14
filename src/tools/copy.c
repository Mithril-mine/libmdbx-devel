/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// mdbx_copy.c - memory-mapped database backup tool
///

#define xMDBX_TOOLS /* Avoid using internal ASSERT(), etc */
#include "essentials.h"

#include "tools/common.h"

#if IS_WINDOWS

/* Bit of madness for Windows console */

#include "wingetopt.h"

#endif /* WINDOWS */

static const char *prog;
static void usage_msg(FILE *out) {
  fprintf(out,
          "usage: %s [-V] [-q] [-c] [-d] [-f] [-p] [-u|U] [-n] [-h|--help] src_path [dest_path]\n"
          "  -V\t\tprint version and exit\n"
          "  -q\t\tbe quiet\n"
          "  -c\t\tenable compactification (skip unused pages)\n"
          "  -f\t\tforce copying even the target file exists\n"
          "  -d\t\tenforce copy to be a dynamic size DB\n"
          "  -p\t\tusing transaction parking/ousting during copying MVCC-snapshot\n"
          "    \t\tto avoid stopping recycling and overflowing the DB\n"
          "  -u\t\twarmup database before copying\n"
          "  -U\t\twarmup and try lock database pages in memory before copying\n"
          "  -n\t\topen MDBX environment(s) which do not use subdirectories\n"
          "  -h|--help\tprint this usage message and exit\n"
          "  src_path\tsource database\n"
          "  dest_path\tdestination (stdout if not specified)\n",
          prog);
}

static void usage(void) {
  usage_msg(stderr);
  exit(EXIT_FAILURE);
}

static void help(void) {
  usage_msg(stdout);
  exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
  int rc;
  MDBX_env *env = nullptr;
  const char *act;
  unsigned flags = MDBX_RDONLY;
  unsigned cpflags = 0;
  bool quiet = false;
  bool warmup = false;
  MDBX_warmup_flags_t warmup_flags = MDBX_warmup_default;

  prog = argv[0];
  if (tool_help_requested(argc, argv, "VqcfdpnuUnh"))
    help();

  for (; argc > 1 && argv[1][0] == '-'; argc--, argv++) {
    if (argv[1][1] == 'n' && argv[1][2] == '\0')
      flags |= MDBX_NOSUBDIR;
    else if (argv[1][1] == 'c' && argv[1][2] == '\0')
      cpflags |= MDBX_CP_COMPACT;
    else if (argv[1][1] == 'd' && argv[1][2] == '\0')
      cpflags |= MDBX_CP_FORCE_DYNAMIC_SIZE;
    else if (argv[1][1] == 'p' && argv[1][2] == '\0')
      cpflags |= MDBX_CP_THROTTLE_MVCC;
    else if (argv[1][1] == 'f' && argv[1][2] == '\0')
      cpflags |= MDBX_CP_OVERWRITE;
    else if (argv[1][1] == 'q' && argv[1][2] == '\0')
      quiet = true;
    else if (argv[1][1] == 'u' && argv[1][2] == '\0')
      warmup = true;
    else if (argv[1][1] == 'U' && argv[1][2] == '\0') {
      warmup = true;
      warmup_flags = MDBX_warmup_force | MDBX_warmup_touchlimit | MDBX_warmup_lock;
    } else if (argv[1][1] == 'V' && argv[1][2] == '\0') {
      tool_version_print("copy");
      return EXIT_SUCCESS;
    } else
      argc = 0;
  }

  if (argc < 2 || argc > 3)
    usage();

  tool_break_setup();

  if (!quiet) {
    fprintf((argc == 2) ? stderr : stdout, "mdbx_copy %s (%s, T-%s)\nRunning for copy %s to %s...\n",
            mdbx_version.git.describe, mdbx_version.git.datetime, mdbx_version.git.tree, argv[1],
            (argc == 2) ? "stdout" : argv[2]);
    fflush(nullptr);
    mdbx_setup_debug(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, tool_logger_default);
  }

  act = "opening environment";
  rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, argv[1], flags, 0);

  if (rc == MDBX_SUCCESS && warmup) {
    act = "warming up";
    rc = mdbx_env_warmup(env, nullptr, warmup_flags, 3600 * 65536);
  }

  if (!MDBX_IS_ERROR(rc)) {
    act = "copying";
    if (argc == 2) {
      mdbx_filehandle_t fd;
#if IS_WINDOWS
      fd = GetStdHandle(STD_OUTPUT_HANDLE);
#else
      fd = fileno(stdout);
#endif
      rc = mdbx_env_copy2fd(env, fd, cpflags);
    } else
      rc = mdbx_env_copy(env, argv[2], cpflags);
  }
  if (rc)
    fprintf(stderr, "%s: %s failed, error %d (%s)\n", prog, act, rc, mdbx_strerror(rc));
  mdbx_env_close(env);

  return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
