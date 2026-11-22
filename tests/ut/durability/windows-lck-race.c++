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

/// The stress attempt to reproduce the `windows-lck-restored-copy-opener-race`
/// finding of the verification campaign (#485, follow-up issue #549, model-
/// verified only, never reproduced at runtime).
///
/// The claimed mechanics (lck-posix.c:285-418 at the campaign time, the same
/// lines today): with the lck file replaced by a restored COPY of a previously
/// captured lck (valid magic/format/size, which bypasses the "too short"
/// check_fstat defusal), two racing openers can steer the second one into the
/// shared-return path of lck_seize(): opener A holds the byte0 RDLCK (after
/// its exclusive-dxb attempt collided with the old world), so opener B's
/// exclusive retry fails with a sharing violation and B returns
/// MDBX_RESULT_FALSE at lck-posix.c:417 WITHOUT ever taking the dxb exclusive
/// lock. Two coordination worlds then coexist and their writers may corrupt
/// the database silently. A FRESH (empty) replacement is defused by the
/// 2-second dxb-collision deadline (lck-posix.c:357-361), since the old
/// world's persistent dxb byte-pid locks (lck-posix.c:409) block every
/// new-world exclusive seize.
///
/// On Windows the scenario is only approachable in a weakened form, and this
/// stress documents why by construction:
///  - the lck file cannot be replaced while a live env keeps it open (no
///    FILE_SHARE_DELETE in the MDBX_OPEN_LCK ShareMode, see
///    src/osal/windows/osal-file.c), so the old world must be closed before
///    the restore - whereas the model requires it to be alive, with its
///    persistent dxb locks steering the openers into the shared-return path;
///  - the Windows seize FSM (src/lck-windows.c internal_seize_lck) lets the
///    lower half enter the shared state only through the exclusive (E-E) path
///    or after another opener's exclusive seize, so exactly one opener per
///    world always performs the full lck re-initialization (memset at
///    src/lck.c:88), which wipes the restored copy;
///  - there is no 2-second deadline on Windows: the analogous defusal is the
///    immediate LCK_DONTWAIT dxb-whole check (src/lck-windows.c:523-534), and
///    the lck replacement itself is not detected on Windows at all (the
///    fileid re-verification at src/lck.c:206 is POSIX-only).
///
/// Therefore the race is expected NOT to fire; this stress is best-effort
/// evidence collection: it loops iterations of "capture the live lck, close
/// the world, restore the copy (or a fresh empty replacement as the control),
/// race two opener processes with tightly controlled launch offsets swept
/// around the deadline windows", and after each iteration verifies the
/// database integrity end-to-end (mdbx_env_chk() plus a full read pass with
/// exact values). A corruption between two coordination worlds would surface
/// as a failed check, missing values of a successfully committed opener or
/// unexpected keys. Every iteration's outcome is logged to stderr.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <windows.h>

namespace {

constexpr unsigned kDefaultIterations = 200;
constexpr unsigned kMaxIterations = 5000;
const char *const kIterationsVar = "MDBX_TEST_LCK_RACE_ITERATIONS";
const char *const kOpenerVar = "MDBX_TEST_LCK_RACE_OPENER"; /* "slot:iteration" */
const char *const kDb = "windows-lck-race.mdbx";
const std::string kLck = std::string(kDb) + MDBX_LOCK_SUFFIX;
constexpr unsigned kSeedRecords = 16;
constexpr unsigned kChildRecords = 8;
constexpr unsigned kOpeners = 2;
constexpr DWORD kWaitReadyMs = 15000;
constexpr DWORD kWaitExitMs = 60000;
/* the prompt reap budget for a not-released opener on a broken-setup path */
constexpr DWORD kReapMs = 1000;

/* Launch offsets (ms) between the two openers' permission-to-open events,
 * swept per iteration: back-to-back, the sub-10ms seize windows,
 * TIMEOUT_SHORT_MS (121) of internal_seize_lck(), and the neighborhood of the
 * 2-second defusal deadline (lck-posix.c:360, the finding's window). */
constexpr unsigned kOffsetsMs[] = {0, 1, 4, 16, 64, 121, 300, 700, 1200, 1800, 2100, 2600};

std::string event_name(const char *what, unsigned iteration, unsigned slot) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Local\\mdbx-ut-lck-race-%s-%lu-%u-%u", what, (unsigned long)GetCurrentProcessId(),
                iteration, slot);
  return std::string(buf);
}

bool put_record(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const char *value) {
  MDBX_val k{const_cast<char *>(key), std::strlen(key)}, v{const_cast<char *>(value), std::strlen(value)};
  return mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) == MDBX_SUCCESS;
}

bool write_txns(MDBX_env *env, unsigned slot, unsigned count) {
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  for (unsigned i = 0; i < count; ++i) {
    char key[32], value[64];
    std::snprintf(key, sizeof(key), "c%u-%02u", slot, i);
    std::snprintf(value, sizeof(value), "writer-%u-value-%02u", slot, i);
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      return false;
    if (i == 0 && mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
    const bool done = put_record(txn, dbi, key, value) && mdbx_txn_commit(txn) == MDBX_SUCCESS;
    if (!done) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  return true;
}

/* The spawned opener child: waits for its "go" event, opens the environment
 * as fast as possible, commits its write-transaction sequence and exits.
 * The exit code reports the outcome: 0 - the environment was opened and all
 * the writes were committed; 3 - the open failed; 4 - a write failed;
 * 5 - an internal setup failure. The files are owned by the parent, so the
 * child must only close its own handles on every exit path. */
void run_opener_child(unsigned slot, unsigned iteration) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(5);
  HANDLE go = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name("go", iteration, slot).c_str());
  if (!go) {
    mdbx_env_close(env);
    _exit(5);
  }
  const DWORD waited = WaitForSingleObject(go, kWaitReadyMs);
  CloseHandle(go);
  if (waited != WAIT_OBJECT_0) {
    mdbx_env_close(env);
    _exit(5);
  }
  const int open_rc = mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644);
  fprintf(stderr, "lck-race: opener-%u: open rc %d (%s)\n", slot, open_rc, mdbx_strerror(open_rc));
  fflush(stderr);
  if (open_rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    _exit(3);
  }
  if (!write_txns(env, slot, kChildRecords)) {
    mdbx_env_close(env);
    _exit(4);
  }
  if (mdbx_env_close(env) != MDBX_SUCCESS)
    _exit(4);
  _exit(0);
}

/* A spawned opener process together with its handshake events. */
struct opener_process {
  PROCESS_INFORMATION pi{};
  HANDLE ready = nullptr, go = nullptr;
  bool spawned = false;

  bool start(unsigned iteration, unsigned slot) {
    ready = CreateEventA(nullptr, TRUE, FALSE, event_name("ready", iteration, slot).c_str());
    go = CreateEventA(nullptr, TRUE, FALSE, event_name("go", iteration, slot).c_str());
    if (!ready || !go) {
      fprintf(stderr, "lck-race: iter %u: CreateEventA() failed for slot %u, error %lu\n", iteration, slot,
              GetLastError());
      return false;
    }
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
      fprintf(stderr, "lck-race: iter %u: GetModuleFileNameA() failed, error %lu\n", iteration, GetLastError());
      return false;
    }
    std::string cmdline = "\"" + std::string(exe) +
                          "\" --gtest_filter=windows_lck_race.DISABLED_opener"
                          " --gtest_also_run_disabled_tests --gtest_brief=1";
    char arg[24];
    std::snprintf(arg, sizeof(arg), "%u:%u", slot, iteration);
    if (!SetEnvironmentVariableA(kOpenerVar, arg)) {
      fprintf(stderr, "lck-race: iter %u: SetEnvironmentVariableA() failed, error %lu\n", iteration, GetLastError());
      return false;
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    const BOOL started = CreateProcessA(exe, &cmdline[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    const DWORD start_error = GetLastError();
    SetEnvironmentVariableA(kOpenerVar, nullptr);
    if (!started) {
      fprintf(stderr, "lck-race: iter %u: CreateProcessA() failed for slot %u, error %lu\n", iteration, slot,
              start_error);
      return false;
    }
    spawned = true;
    return true;
  }

  /* The child signals "ready" right before waiting for "go", so the parent's
   * offsets measure the phase between the two opens, not the spawn jitter. */
  bool wait_ready(unsigned slot) {
    if (!spawned)
      return false;
    const DWORD got = WaitForSingleObject(ready, kWaitReadyMs);
    if (got != WAIT_OBJECT_0)
      fprintf(stderr, "lck-race: opener-%u not ready in %lums (wait rc %lu)\n", slot, (unsigned long)kWaitReadyMs,
              (unsigned long)got);
    return got == WAIT_OBJECT_0;
  }

  /* Releases the opener (unless already released), waits for the exit and
   * returns it; a hung or unreleased opener is reaped promptly and
   * terminated, so a broken-setup iteration cannot burn the budget. */
  DWORD finish(unsigned slot, bool release) {
    DWORD code = (DWORD)-1;
    if (spawned && pi.hProcess) {
      bool exited = false;
      if (release) {
        SetEvent(go);
        exited = WaitForSingleObject(pi.hProcess, kWaitExitMs) == WAIT_OBJECT_0;
      } else
        exited = WaitForSingleObject(pi.hProcess, kReapMs) == WAIT_OBJECT_0;
      if (!exited) {
        fprintf(stderr, "lck-race: opener-%u hung%s, terminating\n", slot, release ? "" : " (unreleased)");
        TerminateProcess(pi.hProcess, 42);
        WaitForSingleObject(pi.hProcess, INFINITE);
      }
      GetExitCodeProcess(pi.hProcess, &code);
      if (pi.hThread)
        CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    pi = PROCESS_INFORMATION{};
    if (ready)
      CloseHandle(ready);
    if (go)
      CloseHandle(go);
    ready = go = nullptr;
    spawned = false;
    return code;
  }
};

/* Reads the content of the lck file while the source environment keeps it
 * open (the MDBX_OPEN_LCK ShareMode allows foreign read handles): this is
 * the "copy" for the restored replacement, with valid magic/format/size. */
bool capture_lck(std::vector<uint8_t> &image) {
  HANDLE h = CreateFileA(kLck.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "lck-race: unable open the lck for capture, error %lu\n", GetLastError());
    return false;
  }
  LARGE_INTEGER size;
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (LONGLONG)(16 << 20);
  if (ok) {
    image.resize((size_t)size.QuadPart);
    DWORD got = 0;
    ok = ReadFile(h, image.data(), (DWORD)image.size(), &got, nullptr) && got == image.size();
  }
  CloseHandle(h);
  if (!ok)
    fprintf(stderr, "lck-race: unable capture the lck content\n");
  return ok;
}

/* The restored-copy replacement: a copy of the captured lck is written over
 * the lck path. On Windows the file cannot be replaced while it is open, so
 * the caller must have closed the source environment; the delete-then-write
 * order gives a new file identity, with the in-place overwrite as the
 * fallback for a transient foreign handle (an antivirus, etc). */
bool restore_lck_copy(const std::vector<uint8_t> &image) {
  if (!DeleteFileA(kLck.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    fprintf(stderr, "lck-race: the lck delete for the restore failed, error %lu\n", GetLastError());
  DWORD put = 0;
  HANDLE h = CreateFileA(kLck.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "lck-race: unable write the restored lck, error %lu\n", GetLastError());
    return false;
  }
  const bool ok = WriteFile(h, image.data(), (DWORD)image.size(), &put, nullptr) && put == image.size();
  CloseHandle(h);
  if (!ok)
    fprintf(stderr, "lck-race: unable write the restored lck content\n");
  return ok;
}

/* The fresh (empty) replacement control: the lck file is removed, so the
 * first opener creates a brand-new empty one. */
bool restore_lck_fresh() {
  if (DeleteFileA(kLck.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
    return true;
  fprintf(stderr, "lck-race: the fresh lck delete failed, error %lu\n", GetLastError());
  return false;
}

/* The post-race verification: the environment must reopen cleanly, the
 * mdbx_chk() pass must report no problems, a full read pass must be
 * error-free, all the seed records and the records of every successfully
 * committed opener must be present with exact values, and no unexpected
 * keys may exist. Returns false when a corruption signature was seen. */
bool verify_integrity(unsigned iteration, unsigned committed_mask) {
  MDBX_env *env = nullptr;
  const env_handle_guard env_owner(&env);
  if (mdbx_env_create(&env) != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: the verification env create failed\n", iteration);
    return false;
  }
  const int open_rc = mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644);
  if (open_rc != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, the reopen failed rc %d (%s)\n", iteration, open_rc,
            mdbx_strerror(open_rc));
    return false;
  }

  MDBX_chk_callbacks_t cb;
  std::memset(&cb, 0, sizeof cb); /* a zeroed callbacks instance is silent */
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof ctx);
  const int chk_rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_result, 0);
  if (chk_rc != MDBX_SUCCESS || ctx.result.total_problems != 0) {
    fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, chk rc %d, problems %zu\n", iteration, chk_rc,
            ctx.result.total_problems);
    return false;
  }

  MDBX_txn *txn = nullptr;
  const txn_handle_guard txn_owner(&txn);
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: the verification read txn failed\n", iteration);
    return false;
  }
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: the verification dbi open failed\n", iteration);
    return false;
  }

  unsigned seeds_seen = 0;
  unsigned child_seen[kOpeners] = {0, 0};
  size_t records = 0;
  MDBX_cursor *cursor = nullptr;
  if (mdbx_cursor_open(txn, dbi, &cursor) != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: the verification cursor open failed\n", iteration);
    return false;
  }
  MDBX_val k, v;
  int rc = mdbx_cursor_get(cursor, &k, &v, MDBX_FIRST);
  while (rc == MDBX_SUCCESS) {
    ++records;
    /* the key data is not NUL-terminated: take a local copy for the scan */
    char key[64];
    if (k.iov_len >= sizeof(key)) {
      fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, oversized key\n", iteration);
      mdbx_cursor_close(cursor);
      return false;
    }
    std::memcpy(key, k.iov_base, k.iov_len);
    key[k.iov_len] = '\0';
    unsigned slot = 0, index = 0;
    if (sscanf(key, "c%1u-%2u", &slot, &index) == 2 && slot < kOpeners && index < kChildRecords) {
      char expected[64];
      std::snprintf(expected, sizeof(expected), "writer-%u-value-%02u", slot, index);
      if (v.iov_len != std::strlen(expected) || std::memcmp(v.iov_base, expected, v.iov_len) != 0) {
        fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, wrong value of key %s\n", iteration, key);
        mdbx_cursor_close(cursor);
        return false;
      }
      ++child_seen[slot];
    } else if (sscanf(key, "seed-%2u", &index) == 1 && index < kSeedRecords) {
      char expected[64];
      std::snprintf(expected, sizeof(expected), "seed-value-%02u", index);
      if (v.iov_len != std::strlen(expected) || std::memcmp(v.iov_base, expected, v.iov_len) != 0) {
        fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, wrong value of key %s\n", iteration, key);
        mdbx_cursor_close(cursor);
        return false;
      }
      ++seeds_seen;
    } else {
      fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, unexpected key %s\n", iteration, key);
      mdbx_cursor_close(cursor);
      return false;
    }
    rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT);
  }
  mdbx_cursor_close(cursor);
  if (rc != MDBX_NOTFOUND) {
    fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, the read pass ended with rc %d\n", iteration, rc);
    return false;
  }
  for (unsigned slot = 0; slot < kOpeners; ++slot) {
    if ((committed_mask & (1u << slot)) && child_seen[slot] != kChildRecords) {
      fprintf(stderr,
              "lck-race: iter %u: CORRUPTION signature, data loss: the committed opener-%u left %u of %u records\n",
              iteration, slot, child_seen[slot], kChildRecords);
      return false;
    }
  }
  if (seeds_seen != kSeedRecords) {
    fprintf(stderr, "lck-race: iter %u: CORRUPTION signature, data loss: %u of %u seed records\n", iteration,
            seeds_seen, kSeedRecords);
    return false;
  }
  fprintf(stderr, "lck-race: iter %u: verify ok, %zu records\n", iteration, records);
  return true;
}

struct iteration_result {
  bool corruption = false; /* a corruption signature was seen, stop the stress */
  bool setup_ok = false;   /* the spawn/race phase actually ran */
  unsigned committed_mask = 0;
};

/* One iteration of the stress: the live world A with its lck captured, the
 * restored-copy (or fresh-empty) replacement, two racing opener processes
 * with the swept launch offset, and the end-to-end integrity verification. */
iteration_result run_iteration(unsigned iteration) {
  iteration_result result;

  /* 1. Cleanup of the previous iteration, then the live world A. */
  mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);
  MDBX_env *envA = nullptr;
  env_handle_guard envA_owner(&envA);
  if (mdbx_env_create(&envA) != MDBX_SUCCESS || mdbx_env_open(envA, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    fprintf(stderr, "lck-race: iter %u: the setup env failed\n", iteration);
    return result;
  }
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  for (unsigned i = 0; i < kSeedRecords; ++i) {
    char key[32], value[64];
    std::snprintf(key, sizeof(key), "seed-%02u", i);
    std::snprintf(value, sizeof(value), "seed-value-%02u", i);
    if (mdbx_txn_begin(envA, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS) {
      fprintf(stderr, "lck-race: iter %u: the setup seed txn failed\n", iteration);
      return result;
    }
    if (i == 0 && mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      fprintf(stderr, "lck-race: iter %u: the setup seed dbi failed\n", iteration);
      return result;
    }
    if (!put_record(txn, dbi, key, value) || mdbx_txn_commit(txn) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      fprintf(stderr, "lck-race: iter %u: the setup seed commit failed\n", iteration);
      return result;
    }
  }

  /* 2. The lck capture (valid magic/format/size) while the world is live. */
  std::vector<uint8_t> image;
  if (!capture_lck(image)) {
    fprintf(stderr, "lck-race: iter %u: the lck capture failed\n", iteration);
    return result;
  }

  /* 3. Close the world: on Windows the lck replacement needs no open handles
   * (no FILE_SHARE_DELETE in the MDBX_OPEN_LCK ShareMode). The restored-copy
   * replacement follows; every 4th iteration restores nothing, leaving the
   * fresh empty replacement as the defusal control. */
  envA_owner.release();
  mdbx_env_close(envA);
  envA = nullptr;
  const bool fresh_variant = (iteration % 4 == 3);
  if (!(fresh_variant ? restore_lck_fresh() : restore_lck_copy(image))) {
    fprintf(stderr, "lck-race: iter %u: the %s replacement setup failed\n", iteration,
            fresh_variant ? "fresh" : "restored-copy");
    return result;
  }

  /* 4. Two opener processes with the launch phase controlled exactly: both
   * are spawned first and hold before their open, then the parent releases
   * them with the swept offset in between. */
  opener_process opener[kOpeners];
  for (unsigned slot = 0; slot < kOpeners; ++slot)
    if (!opener[slot].start(iteration, slot)) {
      fprintf(stderr, "lck-race: iter %u: the spawn of opener-%u failed\n", iteration, slot);
      for (unsigned undo = 0; undo < kOpeners; ++undo)
        opener[undo].finish(undo, false);
      return result;
    }
  for (unsigned slot = 0; slot < kOpeners; ++slot)
    if (!opener[slot].wait_ready(slot)) {
      fprintf(stderr, "lck-race: iter %u: the opener-%u readiness failed\n", iteration, slot);
      for (unsigned undo = 0; undo < kOpeners; ++undo)
        opener[undo].finish(undo, false);
      return result;
    }

  const unsigned offset = kOffsetsMs[iteration % (sizeof(kOffsetsMs) / sizeof(kOffsetsMs[0]))];
  SetEvent(opener[0].go);
  Sleep(offset);
  SetEvent(opener[1].go);

  result.committed_mask = 0;
  for (unsigned slot = 0; slot < kOpeners; ++slot) {
    const DWORD code = opener[slot].finish(slot, true);
    fprintf(stderr, "lck-race: opener-%u exited %lu\n", slot, (unsigned long)code);
    if (code == 0)
      result.committed_mask |= (1u << slot);
  }
  result.setup_ok = true;

  /* 5. The end-to-end integrity verification. */
  const bool verified = verify_integrity(iteration, result.committed_mask);
  fprintf(stderr, "lck-race: iter %u: variant=%s offset=%ums committed=%u verify=%s\n", iteration,
          fresh_variant ? "fresh-empty" : "restored-copy", offset, result.committed_mask,
          verified ? "ok" : "CORRUPTED");
  fflush(stderr);
  result.corruption = !verified;
  return result;
}

} // namespace

TEST(windows_lck_race, DISABLED_opener) {
  const char *const spawned = std::getenv(kOpenerVar);
  if (!spawned || !*spawned) {
    GTEST_SKIP() << "runs only as a spawned opener child of restored_copy_two_opener_stress";
    return;
  }
  unsigned slot = 0, iteration = 0;
  if (sscanf(spawned, "%u:%u", &slot, &iteration) != 2 || slot >= kOpeners) {
    GTEST_SKIP() << "malformed opener marker " << spawned;
    return;
  }
  run_opener_child(slot, iteration); /* never returns */
}

TEST(windows_lck_race, restored_copy_two_opener_stress) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  unsigned iterations = kDefaultIterations;
  const char *const configured = std::getenv(kIterationsVar);
  if (configured && *configured) {
    const long parsed = atol(configured);
    if (parsed > 0 && parsed <= (long)kMaxIterations)
      iterations = (unsigned)parsed;
  }

  unsigned exercised = 0, setups_failed = 0, both_committed = 0;
  for (unsigned i = 0; i < iterations; ++i) {
    const iteration_result result = run_iteration(i);
    if (result.corruption) {
      EXPECT_FALSE(result.corruption) << "CORRUPTION signature reproduced at iteration " << i
                                      << ", the stress stops here";
      return;
    }
    if (result.setup_ok) {
      ++exercised;
      if (result.committed_mask == (1u << kOpeners) - 1)
        ++both_committed;
    } else
      ++setups_failed;
  }

  fprintf(stderr, "lck-race: summary: %u iterations, %u exercised, %u setups failed, %u with both openers committed\n",
          iterations, exercised, setups_failed, both_committed);
  fflush(stderr);
  /* The stress must have actually raced: a setup failure on every iteration
   * means the harness itself is broken (spawning, events, capture). */
  EXPECT_GT(exercised, 0u) << "no iteration reached the race phase, the stress is vacuous";
  /* The openers must have actually written: on Windows the seize FSM has no
   * defusal deadline, so a failing opener is a harness symptom, not a
   * defusal - the model's MDBX_BUSY outcome is unreachable here. */
  EXPECT_GT(both_committed, 0u) << "no iteration had both openers committed, the write path was not exercised";
}

#else /* !Windows: the suite is registered for the Windows cells only */

TEST(windows_lck_race, DISABLED_opener) { GTEST_SKIP() << "Windows-only"; }

TEST(windows_lck_race, restored_copy_two_opener_stress) {
  GTEST_SKIP() << "Windows-only stress of the restored-copy lck opener race (#549)";
}

#endif /* Windows */
