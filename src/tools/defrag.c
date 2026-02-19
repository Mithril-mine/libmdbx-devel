/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanations license change,
/// credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026
///
/// mdbx_defrag.c - memory-mapped database defragmentation tool
///

#define xMDBX_TOOLS /* Avoid using internal eASSERT(), etc */
#include "essentials.h"

MDBX_defrag_result_t defrag_result;

#if defined(_WIN32) || defined(_WIN64)

/* Bit of madness for Windows console */
#define mdbx_strerror mdbx_strerror_ANSI2OEM
#define mdbx_strerror_r mdbx_strerror_r_ANSI2OEM

#include "wingetopt.h"

static BOOL WINAPI ConsoleBreakHandlerRoutine(DWORD dwCtrlType) {
  (void)dwCtrlType;
  defrag_result.stopping_reasons |= MDBX_defrag_user_break;
  return true;
}

#else /* WINDOWS */

static void signal_handler(int sig) {
  (void)sig;
  defrag_result.stopping_reasons |= MDBX_defrag_user_break;
}

#endif /* !WINDOWS */

static void usage(const char *progname) {
  fprintf(stderr,
          "usage: %s [-V] [-q] [-c] [-d] [-p] [-u|U] db_pathname\n"
          "  -V\t\tprint version and exit\n"
          "  -v\t\tmore verbose, could be repeated for extra details from debug-enabled  builds\n"
          "  -q\t\tbe quiet\n"
          "  -c\t\tforce cooperative mode (don't try exclusive)\n"
          "  -u\t\twarmup database before defragmenting\n"
          "  -U\t\twarmup and try lock database pages in memory before defragmenting\n"
          "  db_pathname\tpath to the database\n",
          progname);
  exit(EXIT_FAILURE);
}

static void logger(MDBX_log_level_t level, const char *function, int line, const char *fmt, va_list args) {
  static const char *const prefixes[] = {
      "!!!fatal: ",   // 0 fatal
      " ! ",          // 1 error
      " ~ ",          // 2 warning
      "   ",          // 3 notice
      "   // ",       // 4 verbose
      "   //// ",     // 5 verbose
      "   ////// ",   // 6 verbose
      "   //////// ", // 7 verbose
  };
  if (level < MDBX_LOG_DEBUG) {
    if (function && line)
      fprintf(stderr, "%s", prefixes[level]);
    vfprintf(stderr, fmt, args);
  }
}

int main(int argc, char *argv[]) {
  int rc;
  MDBX_env *env = nullptr;
  const char *const progname = argv[0];
  MDBX_log_level_t verbosity = MDBX_LOG_NOTICE;
  bool quiet = false;
  bool warmup = false;
  MDBX_env_flags_t env_flags = MDBX_ENV_DEFAULTS | MDBX_EXCLUSIVE;
  MDBX_warmup_flags_t warmup_flags = MDBX_warmup_default;

  if (argc < 2)
    usage(progname);

  for (int i; (i = getopt(argc, argv,
                          "uU"
                          "V"
                          "v"
                          "q"
                          "c")) != EOF;) {
    switch (i) {
    case 'V':
      printf("mdbx_defarg version %d.%d.%d.%d\n"
             " - source: %s %s, commit %s, tree %s\n"
             " - anchor: %s\n"
             " - build: %s for %s by %s\n"
             " - flags: %s\n"
             " - options: %s\n",
             mdbx_version.major, mdbx_version.minor, mdbx_version.patch, mdbx_version.tweak, mdbx_version.git.describe,
             mdbx_version.git.datetime, mdbx_version.git.commit, mdbx_version.git.tree, mdbx_sourcery_anchor,
             mdbx_build.datetime, mdbx_build.target, mdbx_build.compiler, mdbx_build.flags, mdbx_build.options);
      return EXIT_SUCCESS;
    case 'v':
      if (++verbosity > 9)
        usage(progname);
      break;
    case 'q':
      quiet = true;
      break;
    case 'c':
      env_flags = (env_flags & ~MDBX_EXCLUSIVE) | MDBX_ACCEDE;
      break;
    case 'u':
      warmup = true;
      break;
    case 'U':
      warmup = true;
      warmup_flags = MDBX_warmup_force | MDBX_warmup_touchlimit | MDBX_warmup_lock;
      break;
    default:
      usage(progname);
    }
  }

  if (optind != argc - 1)
    usage(progname);

#if defined(_WIN32) || defined(_WIN64)
  SetConsoleCtrlHandler(ConsoleBreakHandlerRoutine, true);
#else
#ifdef SIGPIPE
  signal(SIGPIPE, signal_handler);
#endif
#ifdef SIGHUP
  signal(SIGHUP, signal_handler);
#endif
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
#endif /* !WINDOWS */

  const char *const db_pathname = argv[optind];
  if (!quiet) {
    fprintf(stdout, "mdbx_defrag %s (%s, T-%s)\nRunning for %s...\n", mdbx_version.git.describe,
            mdbx_version.git.datetime, mdbx_version.git.tree, db_pathname);
    if (verbosity > MDBX_LOG_VERBOSE && !MDBX_DEBUG)
      printf("Verbosity level %u exposures only to"
             " a debug/extra-logging-enabled builds (with NDEBUG undefined"
             " or MDBX_DEBUG > 0)\n",
             verbosity);
    mdbx_setup_debug(verbosity, MDBX_DBG_DONTCHANGE, logger);
    fflush(nullptr);
  }

  const char *act = "opening environment";
  rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, db_pathname, env_flags, 0);
  if ((env_flags & MDBX_EXCLUSIVE) && (rc == MDBX_BUSY ||
#if defined(_WIN32) || defined(_WIN64)
                                       rc == ERROR_LOCK_VIOLATION || rc == ERROR_SHARING_VIOLATION
#else
                                       rc == EBUSY || rc == EAGAIN
#endif
                                       )) {
    env_flags = (env_flags & ~MDBX_EXCLUSIVE) | MDBX_ACCEDE;
    rc = mdbx_env_open(env, db_pathname, env_flags, 0);
  }

  if (rc == MDBX_SUCCESS && warmup) {
    act = "warming up";
    rc = mdbx_env_warmup(env, nullptr, warmup_flags, 3600 * 65536);
  }

  if (!MDBX_IS_ERROR(rc)) {
    act = "defragmenting";
    rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 0, &defrag_result);
  }

  char took_buffer[42];
  switch (rc) {
  default:
    fprintf(stderr, "%s: %s failed, error %d (%s)\n", progname, act, rc, mdbx_strerror(rc));
    break;
  case MDBX_SUCCESS:
  case MDBX_RESULT_TRUE:
    printf("Defragmentation %s: shrinked %zi pages, %u passes, %zu pages moved, stopping reasons bits 0x%x,"
           " took %s seconds\n",
           (rc == MDBX_SUCCESS) ? "done" : "incomplete", defrag_result.shrinked_pages, defrag_result.cycles,
           defrag_result.moved_pages, defrag_result.stopping_reasons,
           mdbx_ratio2digits(defrag_result.spent_time_16dot16, 65536, 3, took_buffer, sizeof(took_buffer)));
  }

  mdbx_env_close(env);
  return MDBX_IS_ERROR(rc) ? EXIT_FAILURE : EXIT_SUCCESS;
}
