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
  int opt;

  prog = argv[0];
  if (tool_help_requested(argc, argv, "VqcfdpnuUnh"))
    help();

  while ((opt = getopt(argc, argv, "VqcfdpnuUnh")) != -1)
    switch (opt) {
    case 'V':
      tool_version_print("copy");
      return EXIT_SUCCESS;
    case 'q':
      quiet = true;
      break;
    case 'c':
      cpflags |= MDBX_CP_COMPACT;
      break;
    case 'd':
      cpflags |= MDBX_CP_FORCE_DYNAMIC_SIZE;
      break;
    case 'p':
      cpflags |= MDBX_CP_THROTTLE_MVCC;
      break;
    case 'f':
      cpflags |= MDBX_CP_OVERWRITE;
      break;
    case 'n':
      flags |= MDBX_NOSUBDIR;
      break;
    case 'u':
      warmup = true;
      break;
    case 'U':
      warmup = true;
      warmup_flags = MDBX_warmup_force | MDBX_warmup_touchlimit | MDBX_warmup_lock;
      break;
    case 'h':
      help();
      break; /* noreturn */
    default:
      usage();
    }

  argc -= optind;
  argv += optind;
  if (argc < 1 || argc > 2)
    usage();

  tool_break_setup();

  if (!quiet) {
    fprintf((argc == 1) ? stderr : stdout, "mdbx_copy %s (%s, T-%s)\nRunning for copy %s to %s...\n",
            mdbx_version.git.describe, mdbx_version.git.datetime, mdbx_version.git.tree, argv[0],
            (argc == 1) ? "stdout" : argv[1]);
    fflush(nullptr);
    mdbx_setup_debug(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, tool_logger_default);
  }

  act = "opening environment";
  rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, argv[0], flags, 0);

  if (rc == MDBX_SUCCESS && warmup) {
    act = "warming up";
    rc = mdbx_env_warmup(env, nullptr, warmup_flags, 3600 * 65536);
  }

  if (user_break) {
    /* the break handlers are installed, so SIGINT/SIGTERM must not be
     * swallowed silently between the stages (chk-interrupt-exit-code) */
    act = "interrupted";
    rc = MDBX_EINTR;
  }

  if (!MDBX_IS_ERROR(rc)) {
    act = "copying";
    if (argc == 1) {
      mdbx_filehandle_t fd;
#if IS_WINDOWS
      fd = GetStdHandle(STD_OUTPUT_HANDLE);
#else
      fd = fileno(stdout);
#endif
      rc = mdbx_env_copy2fd(env, fd, cpflags);
    } else
      rc = mdbx_env_copy(env, argv[1], cpflags);
  }
  if (rc)
    fprintf(stderr, "%s: %s failed, error %d (%s)\n", prog, act, rc, mdbx_strerror(rc));
  else if (user_break)
    fprintf(stderr, "%s: interrupted by signal after copying\n", prog);
  mdbx_env_close(env);

  return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
