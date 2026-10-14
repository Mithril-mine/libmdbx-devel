/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2021-2026
///
/// mdbx_drop.c - memory-mapped database delete tool
///

#define xMDBX_TOOLS /* Avoid using internal ASSERT(), etc */
#include "essentials.h"

#include "tools/common.h"
#include <ctype.h>

#if IS_WINDOWS

/* Bit of madness for Windows console */

#include "wingetopt.h"

#endif /* WINDOWS */

static char *prog;
static bool quiet = false;
static void usage_msg(FILE *out) {
  fprintf(out,
          "usage: %s [-V] [-q] [-d] [-s name] [-h|--help] dbpath\n"
          "  -V\t\tprint version and exit\n"
          "  -q\t\tbe quiet\n"
          "  -d\t\tdelete the specified database, don't just empty it\n"
          "  -s name\tdrop the specified named table\n"
          "  \t\tby default empty the main DB\n"
          "  -h|--help\tprint this usage message and exit\n",
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

static void error(const char *func, int rc) {
  if (!quiet)
    fprintf(stderr, "%s: %s() error %d %s\n", prog, func, rc, mdbx_strerror(rc));
}

int main(int argc, char *argv[]) {
  int i, rc;
  MDBX_env *env;
  MDBX_txn *txn;
  MDBX_dbi dbi;
  char *envname = nullptr;
  char *subname = nullptr;
  int envflags = MDBX_ACCEDE;
  bool delete = false;

  prog = argv[0];
  if (argc < 2)
    usage();

  if (tool_help_requested(argc, argv, "ds:nqVh"))
    help();

  while ((i = getopt(argc, argv,
                     "d"
                     "s:"
                     "n"
                     "q"
                     "V"
                     "h")) != EOF) {
    switch (i) {
    case 'h':
      help();
      break;
    case 'V':
      tool_version_print("drop");
      return EXIT_SUCCESS;
    case 'q':
      quiet = true;
      break;
    case 'd':
      delete = true;
      break;
    case 'n':
      break;
    case 's':
      subname = optarg;
      break;
    default:
      usage();
    }
  }

  if (optind != argc - 1)
    usage();

  tool_break_setup();

  envname = argv[optind];
  if (!quiet) {
    printf("mdbx_drop %s (%s, T-%s)\nRunning for %s/%s...\n", mdbx_version.git.describe, mdbx_version.git.datetime,
           mdbx_version.git.tree, envname, subname ? subname : "@MAIN");
    fflush(nullptr);
    mdbx_setup_debug(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, tool_logger_default);
  }

  rc = mdbx_env_create(&env);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_env_create", rc);
    return EXIT_FAILURE;
  }

  if (subname) {
    rc = mdbx_env_set_maxdbs(env, 2);
    if (unlikely(rc != MDBX_SUCCESS)) {
      error("mdbx_env_set_maxdbs", rc);
      goto env_close;
    }
  }

  rc = mdbx_env_open(env, envname, envflags, 0);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_env_open", rc);
    goto env_close;
  }

  rc = mdbx_txn_begin(env, nullptr, 0, &txn);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_txn_begin", rc);
    goto env_close;
  }

  rc = mdbx_dbi_open(txn, subname, MDBX_DB_ACCEDE, &dbi);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_dbi_open", rc);
    goto txn_abort;
  }

  rc = mdbx_drop(txn, dbi, delete);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_drop", rc);
    goto txn_abort;
  }

  rc = mdbx_txn_commit(txn);
  if (unlikely(rc != MDBX_SUCCESS)) {
    error("mdbx_txn_commit", rc);
    goto txn_abort;
  }
  txn = nullptr;

txn_abort:
  if (txn)
    mdbx_txn_abort(txn);
env_close:
  mdbx_env_close(env);

  return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
