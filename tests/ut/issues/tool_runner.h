/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tool_runner {

/* Directory of the running test executable, so that sibling command-line
 * tools (mdbx_dump, mdbx_load, mdbx_stat) can be located without depending on
 * environment variables, which is unreliable on multi-config Windows builds. */
inline std::string executable_dir() {
#if defined(_WIN32)
  std::vector<char> buf(32768);
  const DWORD n = GetModuleFileNameA(nullptr, buf.data(), (DWORD)buf.size());
  if (n == 0 || n >= buf.size())
    return ".";
  std::string exe(buf.data(), n);
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  _NSGetExecutablePath(buf.data(), &size);
  std::string exe(buf.data(), size);
#else
  std::vector<char> buf(32768);
  const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
  if (n <= 0 || (size_t)n >= buf.size())
    return ".";
  std::string exe(buf.data(), (size_t)n);
#endif
  const size_t slash = exe.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : exe.substr(0, slash);
}

/* Full path to a sibling tool (takes the .exe suffix into account on
 * Windows, where the platform-specific tools are installed next to the
 * test executables). */
inline std::string tool_path(const char *tool) {
  std::string path = executable_dir();
  path += '/';
  path += tool;
#if defined(_WIN32)
  path += ".exe";
#endif
  return path;
}

/* Launch an external tool with an explicit argument vector, optionally
 * redirecting its standard streams to files. The shell is bypassed entirely
 * (fork+execvp on POSIX, CreateProcess on Windows), so command lines with
 * quoted paths behave identically on every platform — unlike std::system(),
 * whose cmd.exe quoting on Windows breaks commands like
 * `"C:\dir\tool.exe" -f "a" "b"`. */
inline int run(const std::vector<std::string> &args, const char *stdin_file = nullptr,
               const char *stdout_file = nullptr, const char *stderr_file = nullptr) {
  std::vector<std::string> owned = args;
  std::vector<char *> argv;
  for (auto &a : owned)
    argv.push_back(&a[0]);
  argv.push_back(nullptr);

#if defined(_WIN32)
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE hIn = stdin_file ? CreateFileA(stdin_file, GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr)
                          : nullptr;
  HANDLE hOut = stdout_file ? CreateFileA(stdout_file, GENERIC_WRITE, FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr)
                            : nullptr;
  HANDLE hErr = stderr_file ? CreateFileA(stderr_file, GENERIC_WRITE, FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr)
                            : nullptr;

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hIn ? hIn : GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = hOut ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = hErr ? hErr : GetStdHandle(STD_ERROR_HANDLE);

  std::string cmdline;
  for (size_t i = 0; i < owned.size(); ++i) {
    if (i)
      cmdline += ' ';
    cmdline += '"';
    for (char c : owned[i])
      if (c == '"')
        cmdline += '\\';
      else
        cmdline += c;
    cmdline += '"';
  }

  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  if (!CreateProcessA(argv[0], &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    return 255;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (hIn)
    CloseHandle(hIn);
  if (hOut)
    CloseHandle(hOut);
  if (hErr)
    CloseHandle(hErr);
  return (int)code;
#else
  pid_t pid = fork();
  if (pid < 0)
    return 255;
  if (pid == 0) {
    if (stdin_file) {
      int fd = ::open(stdin_file, O_RDONLY);
      if (fd < 0 || dup2(fd, STDIN_FILENO) < 0)
        _exit(126);
      ::close(fd);
    }
    if (stdout_file) {
      int fd = ::open(stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
        _exit(126);
      ::close(fd);
    }
    if (stderr_file) {
      int fd = ::open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0 || dup2(fd, STDERR_FILENO) < 0)
        _exit(126);
      ::close(fd);
    }
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return 255;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 255;
#endif
}

} // namespace tool_runner
