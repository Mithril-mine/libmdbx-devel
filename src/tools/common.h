/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026
///
/// Common helpers for the mdbx_* command-line tools.
///
/// This is a header-only module: the amalgamation inlines it into every
/// generated dist/mdbx_*.c (see dist-tool-rule in GNUmakefile), therefore
/// ONLY angle-bracket system includes are allowed here — no `#include "..."`.

#ifndef MDBX_TOOLS_COMMON_H
#define MDBX_TOOLS_COMMON_H

#include <signal.h>
#include <stdio.h>
#include <string.h>

#if IS_WINDOWS

/* Bit of madness for Windows console */
#ifndef mdbx_strerror
#define mdbx_strerror mdbx_strerror_ANSI2OEM
#define mdbx_strerror_r mdbx_strerror_r_ANSI2OEM
#endif

static volatile BOOL user_break;
static BOOL WINAPI ConsoleBreakHandlerRoutine(DWORD dwCtrlType) {
  (void)dwCtrlType;
  user_break = user_break ? 2 : 1;
  return true;
}

static inline void tool_break_setup(void) { SetConsoleCtrlHandler(ConsoleBreakHandlerRoutine, true); }

#else /* !IS_WINDOWS */

static volatile sig_atomic_t user_break;
static void signal_handler(int sig) {
  (void)sig;
  user_break = user_break ? 2 : 1;
}

static inline void tool_break_setup(void) {
#ifdef SIGPIPE
  signal(SIGPIPE, signal_handler);
#endif
#ifdef SIGHUP
  signal(SIGHUP, signal_handler);
#endif
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

#endif /* IS_WINDOWS */

/* Returns true if `-h` or `--help` was requested anywhere in the command
 * line, while honoring the options that consume an argument (per `optstring`),
 * so e.g. `-s --help` is NOT treated as a help request. */
static inline bool tool_help_requested(int argc, char *const argv[], const char *optstring) {
  bool consume_next = false;
  for (int i = 1; i < argc; ++i) {
    if (consume_next) {
      consume_next = false;
      continue;
    }
    const char *const arg = argv[i];
    if (arg[0] != '-' || arg[1] == '\0')
      continue; /* positional argument */
    if (strcmp(arg, "--") == 0)
      break; /* end of options */
    if (strcmp(arg, "--help") == 0)
      return true;
    if (arg[1] == '-')
      continue; /* some other long option, let getopt handle it */
    for (size_t j = 1; arg[j] != '\0'; ++j) {
      const char opt = arg[j];
      if (opt == 'h')
        return true;
      const char *const optdef = strchr(optstring, opt);
      if (!optdef)
        break; /* unknown option, getopt will report it */
      if (optdef[1] == ':') {
        if (arg[j + 1] != '\0')
          break; /* the rest of the cluster is the option argument */
        consume_next = true;
        break;
      }
    }
  }
  return false;
}

/* Uniform version banner (as printed by the `-V` option of each tool). */
static inline void tool_version_print(const char *tool) {
  printf("mdbx_%s version %d.%d.%d.%d\n"
         " - source: %s %s, commit %s, tree %s\n"
         " - anchor: %s\n"
         " - build: %s for %s by %s\n"
         " - flags: %s\n"
         " - options: %s\n",
         tool, mdbx_version.major, mdbx_version.minor, mdbx_version.patch, mdbx_version.tweak,
         mdbx_version.git.describe, mdbx_version.git.datetime, mdbx_version.git.commit, mdbx_version.git.tree,
         mdbx_sourcery_anchor, mdbx_build.datetime, mdbx_build.target, mdbx_build.compiler, mdbx_build.flags,
         mdbx_build.options);
}

/* Default logger shared by the tools that do not need custom formatting. */
static inline void tool_logger_default(MDBX_log_level_t level, const char *function, int line, const char *fmt,
                                       va_list args) {
  static const char *const prefixes[] = {
      "!!!fatal: ", // 0 fatal
      " ! ",        // 1 error
      " ~ ",        // 2 warning
      "   ",        // 3 notice
      "   //",      // 4 verbose
  };
  if (level >= 0 && level < MDBX_LOG_DEBUG) {
    if (function && line && (size_t)level < ARRAY_LENGTH(prefixes))
      fprintf(stderr, "%s", prefixes[level]);
    vfprintf(stderr, fmt, args);
  }
}

#endif /* MDBX_TOOLS_COMMON_H */
