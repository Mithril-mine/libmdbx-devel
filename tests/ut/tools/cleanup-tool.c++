/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// \file Two modes for the test-suite hygiene.
///
/// Default mode: deletes the listed paths through mdbx_env_delete(), for the
/// CTest FIXTURES_CLEANUP tests of the chained smoke scenarios: each chain
/// keeps its database files on purpose (--dont-cleanup-after) for the later
/// tests of the chain, and the cleanup test removes them at the end —
/// including when an intermediate test failed. Nothing to delete is not an
/// error: every listed path is deleted when present, with its `-lck`
/// companion and the directory environment form.
///
/// `--scan` mode (enforcement, item 4 of #343): recursively walks the given
/// directories and reports/fails on ANY leftover database file (`*.mdbx`,
/// `*.mdbx-lck`, `*.db`, `*.db-lck`, `*.db-copy`, `*.db-copy-lck`,
/// `mdbx.dat`, `mdbx.lck`). Intended to run as a separate `ctest -L hygiene`
/// invocation after the whole corpus, when no test is running in parallel.
/// Symbolic-directory links are not followed to avoid scan cycles.

#include "mdbx.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif /* !_WIN32 */

static int delete_mode(int argc, const char *const argv[]) {
  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    const int rc = mdbx_env_delete(argv[i], MDBX_ENV_JUST_DELETE);
    if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
      fprintf(stderr, "cleanup-tool: unable delete `%s`, error %d (%s)\n", argv[i], rc, mdbx_strerror(rc));
      ++failures;
    }
  }
  return failures ? 1 : 0;
}

static int scan_failures = 0;

#ifdef _WIN32

static bool is_database_leftover_w(const wchar_t *name) {
  static const wchar_t *const suffixes[] = {L".mdbx", L".mdbx-lck", L".db", L".db-lck", L".db-copy", L".db-copy-lck"};
  for (const wchar_t *suffix : suffixes) {
    const size_t suffix_length = wcslen(suffix);
    if (wcslen(name) >= suffix_length && wcscmp(name + wcslen(name) - suffix_length, suffix) == 0)
      return true;
  }
  return wcscmp(name, L"mdbx.dat") == 0 || wcscmp(name, L"mdbx.lck") == 0;
}

static bool is_skipped_dir_w(const wchar_t *name) {
  static const wchar_t *const scan_skip_dirs[] = {L"Testing", L"googletest-src", L"googletest-build", L"_deps"};
  for (const wchar_t *skip : scan_skip_dirs)
    if (wcscmp(name, skip) == 0)
      return true;
  return false;
}

static void scan_dir_w(const wchar_t *path) {
  const std::wstring pattern = std::wstring(path) + L"\\*";
  WIN32_FIND_DATAW ffd;
  const HANDLE handle = FindFirstFileW(pattern.c_str(), &ffd);
  if (handle == INVALID_HANDLE_VALUE) {
    fwprintf(stderr, L"cleanup-tool: unable scan `%ls`\n", path);
    ++scan_failures;
    return;
  }
  do {
    if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
      continue;
    const std::wstring full = std::wstring(path) + L"\\" + ffd.cFileName;
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!is_skipped_dir_w(ffd.cFileName))
        scan_dir_w(full.c_str());
    } else if (is_database_leftover_w(ffd.cFileName)) {
      fwprintf(stderr, L"cleanup-tool: leftover database file `%ls`\n", full.c_str());
      ++scan_failures;
    }
  } while (FindNextFileW(handle, &ffd) != 0);
  FindClose(handle);
}

static int scan_mode(int argc, const char *const argv[]) {
  for (int i = 2; i < argc; ++i) { /* argv[1] is the --scan flag itself */
    wchar_t path_wide[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, path_wide, sizeof(path_wide) / sizeof(path_wide[0])) == 0) {
      fprintf(stderr, "cleanup-tool: unable convert `%s` to the wide path\n", argv[i]);
      ++scan_failures;
      continue;
    }
    scan_dir_w(path_wide);
  }
  if (scan_failures) {
    fprintf(stderr, "cleanup-tool: %d leftover database file(s), see above\n", scan_failures);
    return 1;
  }
  printf("cleanup-tool: no leftover database files\n");
  return 0;
}

#else /* POSIX */

static bool is_database_leftover(const char *name, size_t length) {
  static const char *const suffixes[] = {".mdbx", ".mdbx-lck", ".db", ".db-lck", ".db-copy", ".db-copy-lck"};
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    const size_t suffix_length = strlen(suffixes[i]);
    if (length >= suffix_length && strcmp(name + length - suffix_length, suffixes[i]) == 0)
      return true;
  }
  return strcmp(name, "mdbx.dat") == 0 || strcmp(name, "mdbx.lck") == 0;
}

static const char *const scan_skip_dirs[] = {"Testing", "googletest-src", "googletest-build", "_deps"};

static void scan_dir(const char *path, unsigned depth) {
  if (depth > 8)
    return;
  DIR *dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "cleanup-tool: unable scan `%s`\n", path);
    ++scan_failures;
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char full[4096];
    if (snprintf(full, sizeof(full), "%s/%s", path, entry->d_name) >= (int)sizeof(full))
      continue;
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      bool skip = false;
      for (size_t i = 0; i < sizeof(scan_skip_dirs) / sizeof(scan_skip_dirs[0]); ++i)
        if (strcmp(entry->d_name, scan_skip_dirs[i]) == 0) {
          skip = true;
          break;
        }
      if (!skip)
        scan_dir(full, depth + 1);
    } else if (is_database_leftover(entry->d_name, strlen(entry->d_name))) {
      fprintf(stderr, "cleanup-tool: leftover database file `%s`\n", full);
      ++scan_failures;
    }
  }
  closedir(dir);
}

static int scan_mode(int argc, const char *const argv[]) {
  for (int i = 2; i < argc; ++i) /* argv[1] is the --scan flag itself */
    scan_dir(argv[i], 0);
  if (scan_failures) {
    fprintf(stderr, "cleanup-tool: %d leftover database file(s), see above\n", scan_failures);
    return 1;
  }
  printf("cleanup-tool: no leftover database files\n");
  return 0;
}

#endif /* _WIN32 */
int main(int argc, const char *const argv[]) {
  if (argc > 1 && strcmp(argv[1], "--scan") == 0)
    return scan_mode(argc, argv);
  return delete_mode(argc, argv);
}
