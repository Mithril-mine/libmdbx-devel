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

#include "test.h++"

#if IS_WINDOWS

/* Map a result from an NTAPI call to WIN32 error code. */
static int osal_ntstatus2errcode(NTSTATUS status) {
  DWORD dummy;
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.Internal = status;
  /* Zap: '_Param_(1)' could be '0' */
  MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6387);
  return GetOverlappedResult(nullptr, &ov, &dummy, FALSE) ? MDBX_SUCCESS : (int)GetLastError();
}

int osal_waitstatus2errcode(DWORD result) {
  switch (result) {
  case WAIT_OBJECT_0:
    return MDBX_SUCCESS;
  case WAIT_FAILED:
    return (int)GetLastError();
  case WAIT_ABANDONED:
    return ERROR_ABANDONED_WAIT_0;
  case WAIT_IO_COMPLETION:
    return ERROR_USER_APC;
  case WAIT_TIMEOUT:
    return ERROR_TIMEOUT;
  default:
    return osal_ntstatus2errcode(result);
  }
}

static std::unordered_map<unsigned, HANDLE> events;
static HANDLE hBarrierShared, hBarrierEvent, hReadySemaphore;
static volatile LONG *barrier_countdown;
static HANDLE hProgressActiveEvent, hProgressPassiveEvent;

void osal_wait4barrier(void) {
  /* Real counting barrier in shared memory (mirrors the POSIX1988 flavor):
   * each actor InterlockedDecrement()s the countdown; the LAST one to reach 0
   * sets hBarrierEvent, everyone else waits on it (manual-reset event stays
   * signaled, so all proceed together). */
  if (InterlockedDecrement(barrier_countdown) > 0) {
    if (WaitForSingleObject(hBarrierEvent, INFINITE) != WAIT_OBJECT_0)
      failure_perror("WaitForSingleObject(BarrierEvent)", osal_waitstatus2errcode(GetLastError()));
  } else {
    if (!SetEvent(hBarrierEvent))
      failure_perror("SetEvent(BarrierEvent)", GetLastError());
  }
  /* Report readiness to the overlord (which does NOT participate in the
   * barrier anymore): every actor posts exactly once, so acquiring `need`
   * times means "everyone is at the start line". */
  if (!ReleaseSemaphore(hReadySemaphore, 1, nullptr))
    failure_perror("ReleaseSemaphore(ReadySemaphore)", GetLastError());
}

static HANDLE make_inheritable(HANDLE hHandle) {
  assert(hHandle != NULL && hHandle != INVALID_HANDLE_VALUE);
  if (!DuplicateHandle(GetCurrentProcess(), hHandle, GetCurrentProcess(), &hHandle, 0, TRUE,
                       DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
    failure_perror("DuplicateHandle()", GetLastError());
  return hHandle;
}

/* Wait until all `need` actors have posted the ready semaphore, bounded by the
 * --timeout deadline (chrono::infinite() = wait forever). Returns false if the
 * deadline passed first, true when everyone is ready. */
bool osal_wait4ready(size_t need, uint64_t deadline_fixedpoint) {
  for (size_t i = 0; i < need; ++i) {
    DWORD timeout_ms = INFINITE;
    if (deadline_fixedpoint != chrono::infinite().fixedpoint) {
      const uint64_t now = chrono::now_monotonic().fixedpoint;
      if (now >= deadline_fixedpoint)
        return false;
      const uint64_t left = deadline_fixedpoint - now;
      const uint64_t left_ms = ((left >> 32) * 1000) + (((left & 0xffffffffu) * 1000u) >> 32);
      timeout_ms = (left_ms >= INFINITE) ? INFINITE : (DWORD)left_ms;
    }
    if (WaitForSingleObject(hReadySemaphore, timeout_ms) != WAIT_OBJECT_0)
      return false;
  }
  return true;
}

void osal_setup(const std::vector<actor_config> &actors) {
  assert(events.empty());
  const size_t n = actors.size() + 1;
  events.reserve(n);

  for (unsigned i = 0; i < n; ++i) {
    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hEvent)
      failure_perror("CreateEvent()", GetLastError());
    hEvent = make_inheritable(hEvent);
    log_trace("osal_setup: event %u -> %p", i, hEvent);
    events[i] = hEvent;
  }

  hBarrierShared = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG), NULL);
  if (!hBarrierShared)
    failure_perror("CreateFileMapping(BarrierShared)", GetLastError());
  hBarrierShared = make_inheritable(hBarrierShared);
  barrier_countdown = (volatile LONG *)MapViewOfFile(hBarrierShared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
  if (!barrier_countdown)
    failure_perror("MapViewOfFile(BarrierShared)", GetLastError());
  *barrier_countdown = (LONG)actors.size();

  hBarrierEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!hBarrierEvent)
    failure_perror("CreateEvent(BarrierEvent)", GetLastError());
  hBarrierEvent = make_inheritable(hBarrierEvent);

  hReadySemaphore = CreateSemaphoreW(NULL, 0, (LONG)actors.size(), NULL);
  if (!hReadySemaphore)
    failure_perror("CreateSemaphore(ReadySemaphore)", GetLastError());
  hReadySemaphore = make_inheritable(hReadySemaphore);

  hProgressActiveEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!hProgressActiveEvent)
    failure_perror("CreateEvent(ProgressActiveEvent)", GetLastError());
  hProgressActiveEvent = make_inheritable(hProgressActiveEvent);

  hProgressPassiveEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!hProgressPassiveEvent)
    failure_perror("CreateEvent(ProgressPassiveEvent)", GetLastError());
  hProgressPassiveEvent = make_inheritable(hProgressPassiveEvent);
}

void osal_broadcast(unsigned id) {
  log_trace("osal_broadcast: event %u", id);
  if (!SetEvent(events.at(id)))
    failure_perror("SetEvent()", GetLastError());
}

int osal_waitfor(unsigned id) {
  log_trace("osal_waitfor: event %u", id);
  DWORD rc = WaitForSingleObject(events.at(id), INFINITE);
  return osal_waitstatus2errcode(rc);
}

int osal_delay(unsigned seconds) {
  Sleep(seconds * 1000u);
  return 0;
}

//-----------------------------------------------------------------------------

const std::string actor_config::osal_serialize(simple_checksum &checksum) const {
  checksum.push(hBarrierShared);
  checksum.push(hBarrierEvent);
  checksum.push(hReadySemaphore);
  checksum.push(hProgressActiveEvent);
  checksum.push(hProgressPassiveEvent);

  HANDLE hWait = INVALID_HANDLE_VALUE;
  if (wait4id) {
    hWait = events.at(wait4id);
    checksum.push(hWait);
  }

  HANDLE hSignal = INVALID_HANDLE_VALUE;
  if (wanna_event4signalling()) {
    hSignal = events.at(actor_id);
    checksum.push(hSignal);
  }

  return format("%p.%p.%p.%p.%p.%p.%p", hBarrierShared, hBarrierEvent, hReadySemaphore, hWait, hSignal,
                hProgressActiveEvent, hProgressPassiveEvent);
}

bool actor_config::osal_deserialize(const char *str, const char *end, simple_checksum &checksum) {

  std::string copy(str, end - str);
  TRACE(">> osal_deserialize(%s)\n", copy.c_str());

  assert(hBarrierShared == 0);
  assert(hBarrierEvent == 0);
  assert(hReadySemaphore == 0);
  assert(hProgressActiveEvent == 0);
  assert(hProgressPassiveEvent == 0);
  assert(events.empty());

  HANDLE hWait, hSignal;
  if (sscanf_s(copy.c_str(), "%p.%p.%p.%p.%p.%p.%p", &hBarrierShared, &hBarrierEvent, &hReadySemaphore, &hWait,
               &hSignal, &hProgressActiveEvent, &hProgressPassiveEvent) != 7) {
    TRACE("<< osal_deserialize: failed\n");
    return false;
  }
  barrier_countdown = (volatile LONG *)MapViewOfFile(hBarrierShared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
  if (!barrier_countdown) {
    TRACE("<< osal_deserialize: MapViewOfFile(BarrierShared) failed\n");
    return false;
  }

  checksum.push(hBarrierShared);
  checksum.push(hBarrierEvent);
  checksum.push(hReadySemaphore);
  checksum.push(hProgressActiveEvent);
  checksum.push(hProgressPassiveEvent);

  if (wait4id) {
    checksum.push(hWait);
    events[wait4id] = hWait;
  }

  if (wanna_event4signalling()) {
    checksum.push(hSignal);
    events[actor_id] = hSignal;
  }

  TRACE("<< osal_deserialize: OK\n");
  return true;
}

//-----------------------------------------------------------------------------

typedef std::pair<HANDLE, actor_status> child;
static std::unordered_map<mdbx_pid_t, child> children;
static std::mutex children_mutex;

static bool children_empty(void) {
  std::lock_guard<std::mutex> lock(children_mutex);
  return children.empty();
}

static size_t children_size(void) {
  std::lock_guard<std::mutex> lock(children_mutex);
  return children.size();
}

static void children_store(mdbx_pid_t pid, child value) {
  std::lock_guard<std::mutex> lock(children_mutex);
  children[pid] = value;
}

static child children_at(mdbx_pid_t pid) {
  std::lock_guard<std::mutex> lock(children_mutex);
  return children.at(pid);
}

static void children_set_status(mdbx_pid_t pid, actor_status status) {
  std::lock_guard<std::mutex> lock(children_mutex);
  children.at(pid).second = status;
}

static std::vector<std::pair<mdbx_pid_t, child>> children_snapshot(void) {
  std::lock_guard<std::mutex> lock(children_mutex);
  return std::vector<std::pair<mdbx_pid_t, child>>(children.begin(), children.end());
}

bool osal_multiactor_mode(void) { return hProgressActiveEvent || hProgressPassiveEvent; }

bool osal_progress_push(bool active) {
  if (!children_empty()) {
    if (!SetEvent(active ? hProgressActiveEvent : hProgressPassiveEvent))
      failure_perror("osal_progress_push: SetEvent(overlord.progress)", GetLastError());
    return true;
  }

  return false;
}

static void ArgvQuote(std::string &CommandLine, const std::string &Argument, bool Force = false)

/*++

https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/

Routine Description:

    This routine appends the given argument to a command line such
    that CommandLineToArgvW will return the argument string unchanged.
    Arguments in a command line should be separated by spaces; this
    function does not add these spaces.

Arguments:

    Argument - Supplies the argument to encode.

    CommandLine - Supplies the command line to which we append the encoded
argument string.

    Force - Supplies an indication of whether we should quote
            the argument even if it does not contain any characters that would
            ordinarily require quoting.

Return Value:

    None.

Environment:

    Arbitrary.

--*/

{
  //
  // Unless we're told otherwise, don't quote unless we actually
  // need to do so --- hopefully avoid problems if programs won't
  // parse quotes properly
  //

  if (Force == false && Argument.empty() == false && Argument.find_first_of(" \t\n\v\"") == Argument.npos) {
    CommandLine.append(Argument);
  } else {
    CommandLine.push_back('"');

    for (auto It = Argument.begin();; ++It) {
      size_t NumberBackslashes = 0;

      while (It != Argument.end() && *It == '\\') {
        ++It;
        ++NumberBackslashes;
      }

      if (It == Argument.end()) {
        //
        // Escape all backslashes, but let the terminating
        // double quotation mark we add below be interpreted
        // as a metacharacter.
        //
        CommandLine.append(NumberBackslashes * 2, '\\');
        break;
      } else if (*It == L'"') {
        //
        // Escape all backslashes and the following
        // double quotation mark.
        //
        CommandLine.append(NumberBackslashes * 2 + 1, '\\');
        CommandLine.push_back(*It);
      } else {
        //
        // Backslashes aren't special here.
        //
        CommandLine.append(NumberBackslashes, '\\');
        CommandLine.push_back(*It);
      }
    }

    CommandLine.push_back('"');
  }
}

int osal_actor_start(const actor_config &config, mdbx_pid_t &pid) {
  if (children_size() == MAXIMUM_WAIT_OBJECTS)
    failure("Couldn't manage more that %u actors on Windows\n", MAXIMUM_WAIT_OBJECTS);

  _flushall();

  STARTUPINFOA StartupInfo;
  GetStartupInfoA(&StartupInfo);

  char exename[_MAX_PATH + 1];
  DWORD exename_size = sizeof(exename);
  if (!QueryFullProcessImageNameA(GetCurrentProcess(), 0, exename, &exename_size))
    failure_perror("QueryFullProcessImageName()", GetLastError());

  if (exename[1] != ':') {
    exename_size = GetModuleFileNameA(NULL, exename, sizeof(exename));
    if (exename_size >= sizeof(exename))
      return ERROR_BAD_LENGTH;
  }

  std::string cmdline = "$ ";
  ArgvQuote(cmdline, thunk_param(config));

  if (cmdline.size() >= 32767)
    return ERROR_BAD_LENGTH;

  PROCESS_INFORMATION ProcessInformation;
  if (!CreateProcessA(exename, const_cast<char *>(cmdline.c_str()),
                      NULL, // Retuned process handle is not inheritable.
                      NULL, // Retuned thread handle is not inheritable.
                      TRUE, // Child inherits all inheritable handles.
                      NORMAL_PRIORITY_CLASS | INHERIT_PARENT_AFFINITY,
                      NULL, // Inherit the parent's environment.
                      NULL, // Inherit the parent's current directory.
                      &StartupInfo, &ProcessInformation))
    failure_perror(exename, GetLastError());

  CloseHandle(ProcessInformation.hThread);
  pid = ProcessInformation.dwProcessId;
  children_store(pid, std::make_pair(ProcessInformation.hProcess, as_running));
  return 0;
}

actor_status osal_actor_info(const mdbx_pid_t pid) {
  actor_status status = children_at(pid).second;
  if (status > as_running)
    return status;

  DWORD ExitCode;
  if (!GetExitCodeProcess(children_at(pid).first, &ExitCode))
    failure_perror("GetExitCodeProcess()", GetLastError());

  switch (ExitCode) {
  case STILL_ACTIVE:
    return as_running;
  case EXIT_SUCCESS:
    status = as_successful;
    break;
  case EXCEPTION_BREAKPOINT:
  case EXCEPTION_SINGLE_STEP:
    status = as_debugging;
    break;
  case STATUS_CONTROL_C_EXIT:
  case /* STATUS_INTERRUPTED */ 0xC0000515L:
    status = as_killed;
    break;
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_INVALID_DISPOSITION:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
  case /* STATUS_STACK_BUFFER_OVERRUN, STATUS_BUFFER_OVERFLOW_PREVENTED */
      0xC0000409L:
  case /* STATUS_ASSERTION_FAILURE */ 0xC0000420L:
  case /* STATUS_HEAP_CORRUPTION */ 0xC0000374L:
  case /* STATUS_CONTROL_STACK_VIOLATION */ 0xC00001B2L:
    log_error("pid %zu, exception 0x%x", (intptr_t)pid, (unsigned)ExitCode);
    status = as_coredump;
    break;
  default:
    log_error("pid %zu, exit code %u", (intptr_t)pid, (unsigned)ExitCode);
    status = as_failed;
    break;
  }

  children_set_status(pid, status);
  return status;
}

void osal_killall_actors(void) {
  for (const auto &pair : children_snapshot())
    TerminateProcess(pair.second.first, STATUS_CONTROL_C_EXIT);
}

void osal_killall_actors_immediate(void) { osal_killall_actors(); }

std::vector<mdbx_pid_t> osal_actor_pids(void) {
  std::vector<mdbx_pid_t> result;
  for (const auto &pair : children_snapshot())
    result.push_back(pair.first);
  return result;
}

int osal_actor_poll(mdbx_pid_t &pid, unsigned timeout) {
  std::vector<HANDLE> handles;
  const auto children_snap = children_snapshot();
  handles.reserve(children_snap.size() + 2);
  handles.push_back(hProgressActiveEvent);
  handles.push_back(hProgressPassiveEvent);
  for (const auto &pair : children_snap)
    if (pair.second.second <= as_running)
      handles.push_back(pair.second.first);

  while (true) {
    DWORD rc =
        MsgWaitForMultipleObjectsEx((DWORD)handles.size(), &handles[0], (timeout > 60) ? 60 * 1000 : timeout * 1000,
                                    QS_ALLINPUT | QS_ALLPOSTMESSAGE, 0);

    if (rc == WAIT_OBJECT_0) {
      logging::progress_canary(true);
      continue;
    }
    if (rc == WAIT_OBJECT_0 + 1) {
      logging::progress_canary(false);
      continue;
    }

    if (rc >= WAIT_OBJECT_0 + 2 && rc < WAIT_OBJECT_0 + handles.size()) {
      pid = 0;
      for (const auto &pair : children_snap)
        if (pair.second.first == handles[rc - WAIT_OBJECT_0]) {
          pid = pair.first;
          break;
        }
      return 0;
    }

    if (rc == WAIT_TIMEOUT) {
      pid = 0;
      return 0;
    }

    return osal_waitstatus2errcode(rc);
  }
}

void osal_udelay(size_t us) {
  chrono::time until, now = chrono::now_monotonic();
  until.fixedpoint = now.fixedpoint + chrono::from_us(us).fixedpoint;

  static size_t threshold_us;
  if (threshold_us == 0) {
    unsigned timeslice_ms = 1;
    while (timeBeginPeriod(timeslice_ms) == TIMERR_NOCANDO)
      ++timeslice_ms;
    threshold_us = timeslice_ms * size_t(1500);
    assert(threshold_us > 0);
  }

  do {
    if (us > threshold_us && us > 1000) {
      DWORD rc = SleepEx(unsigned(us / 1000), TRUE);
      if (rc)
        failure_perror("SleepEx()", osal_waitstatus2errcode(rc));
      us = 0;
    }

    YieldProcessor();
    now = chrono::now_monotonic();
  } while (now.fixedpoint < until.fixedpoint);
}

bool osal_istty(int fd) { return _isatty(fd) != 0; }

std::string osal_tempdir(void) {
  char buf[MAX_PATH + 1];
  DWORD len = GetTempPathA(sizeof(buf), buf);
  return std::string(buf, len);
}

#endif /* Windows */
