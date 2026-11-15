# Verification wave 7 (stream A top-up, coordination) — proposed doc additions

Per-function **statement-level** ledger for the S9 coordination files whose
wave-4 verdicts were clustered: `src/lck.c` (+ the POSIX engines
`src/lck-posix.c` for the `lck_*` functions named in the task), `src/rthc.c`,
`src/lck-windows.c`, `src/global.c`, `src/env.c`. `src/mvcc-readers.c` is
cross-check-only (wave 4 `doc-additions-v8-b.md` ledgered it per-function).

Reused, not re-proven: INV-S9-1..S9-6 and V-049..V-052 (wave 4),
L2 (`rdt_count` clamp, internals.h:747), PAIR-N10 (cached-oldest distrust),
INV-T8 (unpark base-ABA), V-036 (HSR INT_MAX bound), V-034, L8, safe64
weak/cas split, glibc-21031 workaround, wave-2 env ladders, wave-5 ENV_FATAL
paths, wave-2 machine check of `pv2pages_verify`. Protocol verdicts
`PROVED / PROVED·A / REFUTED / SUSPECT / N-A`; P1–P7 per
`docs/verification/protocol.md`.

**Zero REFUTED.** One new low hardening finding **F-V7C-1** (post-`mdbx_fini`
reuse of the rthc registry is undefended). Everything else confirms the wave-4
clustered verdicts at statement granularity.

---

## 1. `src/lck.c` (setup/decision layer) + POSIX engines for the named `lck_*`

### lck_setup_locked (lck.c:6-128) — PROVED
* P1 neighbors/decision table complete: lck-less → stub + `max_readers=UINT_MAX`
  (all later consumer paths gate on `env->lck_mmap.lck == nullptr`, e.g.
  rthc.c:211/365, so UINT_MAX never indexes an rdt[]). With neighbor:
  `LEGACY_MULTIOPEN==0 → BUSY`; neighbor EXCLUSIVE → BUSY; else legacy-shared
  continue; our exclusive seize downgraded to shared (lck.c:31-36). Without
  neighbor: exclusive → size from OUR `max_readers`; shared + `MDBX_EXCLUSIVE`
  → BUSY (an exclusive env never attaches to a shared world). The
  `MDBX_EACCESS/EPERM`-class gate at lck.c:167 uses ALL-bits `F_ISSET`
  (utils.h:7) — cross-checks INV-S9-4.
* P2 arithmetic: exclusive size `ceil_powerof2(max_readers*sizeof(reader_slot_t)
  + sizeof(lck_t), sys_pagesize)` with `max_readers ∈ [1, 32767]`
  (api-opts.c:261-268) — bounded ≈ 2.4 MiB, no overflow on 32/64-bit. Shared:
  `size` validated `≤ INT_MAX`, page-multiple, `≥ sys_pagesize`; then
  `maxreaders = (size - sizeof(lck_t)) / sizeof(reader_slot_t)` cannot
  underflow (`sizeof(lck_t)` is a few hundred bytes, `size ≥ sys_pagesize
  ≥ 4096` on every supported platform — assumption A-V7C-1); gate `maxreaders
  < 4 → MDBX_PROBLEM`; `env->max_readers = min(maxreaders, MDBX_READERS_LIMIT)`
  (layout-lck.h:297). In shared mode the FILE is authoritative — the user's
  `max_readers` is clobbered by the file-derived value (contract note for
  module-interfaces.md).
* P3/P7: `osal_mmap` SETLENGTH only in the exclusive branch (initializer sizes
  the file), SEMAPHORE in both; `memset(lck,0,size)` + magic before
  `lck_init`; `os_and_format` stamped only AFTER `lck_init` success
  (lck.c:114) — a half-initialized region is refused by every later opener as
  `MDBX_VERSION_MISMATCH` (lck.c:99-103) until the next exclusive opener's
  memset reinitializes it (recovery path verified; see also lck_init row).
* P5: every error return happens before `env->lck = lck` publication
  (lck.c:125); on failure the env stays un-published and the caller's
  `mdbx_env_open` bailout (`api-env.c:558-562`) runs `env_close`, closing the
  lck fd via `lck_destroy`.
* Logging arithmetic: blanket (int/error prints only).

### lck_setup (lck.c:130-239) — PROVED
* P1: open-failure table complete — `EACCESS/EPERM` qualify for lckless only
  with `MDBX_RDONLY|MDBX_EXCLUSIVE` BOTH set (ALL-bits, INV-S9-4/V-049);
  `ENOFILE/EROFS` qualify for `MDBX_RDONLY` after `osal_check_fs_rdonly`
  confirms (`ENOSYS` tolerated only in EXCLUSIVE, lck.c:177); everything else
  propagates. MPROBE_FAULT hook is a no-op outside probe builds (no `#if`).
* P6 publication order: `lck_fileid` published **before** the descriptor,
  separated by `osal_fence_producer()` (lck.c:210-211); `rthc_neighbor_check`
  consumes with an acquire fence (rthc.c:290). POSIX checks neighbors BEFORE
  opening the lck (closing any lck descriptor drops all classic fcntl locks of
  the process — the comment at lck.c:134-139 and G-S9-1); re-check after the
  fd open when the pathname-stat fileid changed (lck.c:206-209) or always on
  Windows.
* P4/P5: seize runs OUTSIDE `rthc_lock()` (lck.c:228-231 — a sibling holding
  the lck exclusively must be able to finish its failing open); the rthc-locked
  bracket only wraps registration/neighbors-check and `lck_setup_locked`.
  Failure after registration leaves the env registered — `env_close` →
  `rthc_remove` → `rthc_drown` → `lck_destroy` closes the fd (P5 closed loop;
  the fd left open at lck.c:223 is reclaimed there).
* Fileid replacement warning (lck.c:207) is log-only; the opened file wins —
  the V-052 trust boundary stands unchanged.

### env_clear_incore_cache (lck.c:241-243) — PROVED
One-line: memset(-1) of a fixed in-struct array; `env->lck` validity is the
caller's (all call sites post-`env_open`-success; verified call graph).

### lck_seize — POSIX (lck-posix.c:285-418) — PROVED·A
Statement ledger of the retry/deadline ladder (P4):
* fork guard `osal_getpid() != env->pid → MDBX_PANIC` (lck-posix.c:287);
  WSL1 refused outright (ENOLCK, data-loss comment).
* without-lck branch: whole-dxb `F_RDLCK/F_WRLCK` via
  `lck_setlk_with3retries`; sharing → `MDBX_BUSY`; success → RESULT_TRUE.
* ladder: byte0 `F_WRLCK` (3 retries) → `check_fstat` → whole-dxb exclusive →
  TRUE. Contention on dxb: bounded by the **2-second deadline**
  `osal_16dot16_to_monotime(2*65536)` (lck-posix.c:357-361) → `MDBX_BUSY`
  (a without-lck dxb holder never goes away by retrying). byte0 contention →
  blocking `F_SETLKW F_RDLCK` (cannot deadlock: seize runs without
  `rthc_lock()`, lck-posix.c:377-381) → `check_fstat == RESULT_TRUE` (files
  truncated by a concurrent `lck_destroy`) → unlock-byte0-and-`retry`;
  got-shared → retry exclusive → sharing → dxb byte-`[pid,1)` lock →
  RESULT_FALSE (shared/operational mode).
* A-ids: A-V7C-2 — the byte0-shared `retry` cycle terminates because
  `check_fstat == TRUE` requires another process to have truncated the files
  under exclusive possession (only `lck_destroy` does that); a hostile
  external truncator could keep the cycle alive — same trust boundary as
  V-052 (external lck-file manipulation), not a defect within the model.
* P5: error returns leave byte0 shared or exclusive held; the caller's open
  failure path closes fds, releasing them (G-S9-1 caveat for classic locks).

### lck_init — POSIX (lck-posix.c:735-898) — PROVED
* P1: initialization happens only for the exclusive initializer
  (`global_uniqueness_flag == RESULT_TRUE`, neighbor-free) — cross-checks
  wave-4 attack 6 (the SysV SETALL cannot interleave: it runs under the seized
  exclusive byte0 lock; the downgrade in env_open happens after).
* SysV: `semget(IPC_CREAT|IPC_EXCL)` → EEXIST → `semget(0)` +
  `sysv_ipc_eexist_decide` (signature in extra semvals + `sem_ctime`
  stability, ≤ 8 attempts then flee; EIDRM/EINVAL → retry/ENOLCK collision)
  → reclaim (`IPC_RMID` → `sysv_retry_create`) or `MDBX_FTOK_COLLISION`
  (ENOLCK). P4: every cycle is bounded by the 8-attempt ctime guard.
* POSIX1988: `sem_init` rdt/wrt process-shared, initializer only.
  POSIX2001/2008: neighbor → skip; FreeBSD non-2008 → always in-process init
  (commented workaround); others initializer-only; attrs pshared, robust
  (2008), `PRIO_INHERIT` with ENOTSUP fallbacks, ERRORCHECK; init rdt then
  wrt. P5: if `wrt_lock` init fails after `rdt_lock` succeeded, the region
  carries magic but no format stamp (see lck_setup_locked P3) → every later
  opener refuses `MDBX_VERSION_MISMATCH`; recovery via the next exclusive
  opener's memset. No path leaves a usable-but-half-initialized shared world.
* `#error "FIXME"` sites fire only for out-of-range `MDBX_LOCKING` — INV-S9-6
  statement-level recheck: all compile-time, none runtime-reachable
  (FUTEX never auto-selected, options.h:397-431).

### lck_destroy — POSIX (lck-posix.c:470-577) — PROVED
* P5 ordering: exclusive re-acquisition (lck whole-file + dxb whole-file, both
  skipped when the fd is INVALID) → drown ipc (SysV `IPC_RMID` with EIDRM/
  EINVAL absorbed; or destroy rdt+wrt ipclocks) → munmap lck → `ftruncate 0`
  (only if nlink>0 and `unsynced_pages==0`) → explicit unlock/close in order
  dxb-then-lck with classic-lock **neighbor restore** (`F_SETLKW`) after each
  close; any restore failure poisons the neighbor with `ENV_FATAL_ERROR`
  (lck-posix.c:574-575). Wave-4 contract rows reproduced statement-level.
* P6 fork guards: `current_pid != env->pid` → skip drown entirely (INV-S9-5);
  the neighbor pointer is nulled for the forked child (lck-posix.c:529-534) so
  no restore is attempted against the parent's fds (the child holds no classic
  locks); fds are still closed unconditionally — matches env_close's
  resurrect path.
* P2: split-unlock ranges `[0,pid)` + `[pid+1, OFF_T_MAX)` and the upgrade
  rollback are non-overlapping and non-underflowing (`pid << OFF_T_MAX` on all
  supported platforms).

### lck_downgrade — POSIX (lck-posix.c:420-440) — PROVED
fcntl split of the full-file unlock leaves exactly `[pid,pid+1)` locked
(wave-4 attack 5, statement-level); then byte0 `F_RDLCK` (3 retries).
Failure mapping sharing→BUSY; EXCLUSIVE envs skip the dxb unlocks (whole-file
locks remain by design).

### lck_upgrade — POSIX (lck-posix.c:442-468) — PROVED
byte0 `F_WRLCK` (blocking per `dont_wait`) → dxb `[0,pid)` → dxb `[pid+1,rest)`
with stepwise rollback (unlock `[0,pid)`, restore byte0 `F_RDLCK`); any
rollback failure → `MDBX_PANIC` (state unknown, env must die). Sharing→BUSY
mapped for portable contention detection (comment).

### lck_rdt_lock / lck_rdt_unlock — POSIX (lck-posix.c:1060-1075) — PROVED
Thin ipclock wrappers on `rdt_lock`. P5: unlock failure → `panic_fmt`
(unrecoverable shared-state violation). mutex_failed/EOWNERDEAD recovery is
in `osal_ipclock_failed` (row below).

### lck_txn_lock / lck_txn_unlock — POSIX (lck-posix.c:1077-1104) — PROVED
* P5/P6: `osal_ipclock_lock(wrt_lock, dont_wait)`; on non-error and
  `basal_txn` — `owner = self` (the assert allows a stale owner only on
  `MDBX_RESULT_TRUE`, i.e. after the robust dead-owner recovery consumed it).
  Unlock: owner assert (`eASSERT1`, checking-builds) + clear **before** the
  mutex unlock; unlock failure → panic.
* mutex_failed recovery statement-level (wave-4 attack 4):
  `osal_ipclock_failed` (lck-posix.c:900-967) — EOWNERDEAD ⇒ owner dead:
  read-lock case runs `mvcc_cleanup_dead(rdt_locked=1)`; write-lock case
  first hoses the env if `env->txn` is set (`ENV_FATAL_ERROR` + PANIC), then
  `cleanup_dead(0)` **before** `pthread_mutex_consistent` (all readers
  re-checked while the mutex is still marked inconsistent — INV-S9-3);
  POSIX2008 additionally unlocks on error. Non-EOWNERDEAD: `EDEADLK` exempt
  from ENV_FATAL (V-051's fail-safe self-relock), everything else →
  `ENV_FATAL_ERROR`.

### osal_ipclock_lock / osal_ipclock_unlock — POSIX (lck-posix.c:988-1058) — PROVED
* P1 per-flavor: 2001/2008 trylock/lock (+bionic tid gate), 1988
  `sem_trywait`→BUSY / `sem_wait` EINTR-loop, SysV `semop` with
  `sem_num = (ipc != &wrt_lock)` (wrt=0, rdt=1), `IPC_NOWAIT`→BUSY;
  success records the pid word (`*ipc ? EOWNERDEAD : SUCCESS; *ipc = pid`).
* P4: EINTR loops only retry on EINTR — terminate when signals cease; SysV
  unlock has an ownership check (`*ipc == env->pid && key != -1` else EPERM).
* P5: unlock failure → fork guard (`current_pid != env->pid → MDBX_SUCCESS`,
  the child never unlocks the parent's lock, INV-S9-5) + leveled logging.

### lck_rpid_set / clear / check — POSIX (lck-posix.c:187-206) — PROVED
* INV-S9-3 statement-level: `lck_rpid_set` places the one-byte `F_WRLCK` at
  offset `env->pid` of the lck file (before the first bind:
  `mvcc_bind_slot` calls it under `lck_rdt_lock` when
  `registered_reader_pid != env->pid`, mvcc-readers.c:25-32) and the lock dies
  with the process; `clear` unlocks the byte; `check` = `F_GETLK` probe:
  `F_UNLCK` ⇒ dead (`RESULT_FALSE`), else live (`RESULT_TRUE`).
* P1: set has an explicit fork guard (`getpid() != env->pid → MDBX_PANIC`).
* V-050 boundary re-confirmed: the `pid > 0` guards here are ASSERTs
  (checking builds only); a crafted negative pid reaches `lck_op` — unchanged
  finding, not re-opened.

### lck_op / lck_setlk_with3retries / osal_lockfile / check_fstat (lck-posix.c:96-185, 232-283) — PROVED
* P1: `EINTR` retried only for non-blocking ops; `cmd == op_setlkw` returns
  the error (no unbounded signal loop). OFD→classic fallback on
  ENOSYS/EINVAL switches the process-global `op_*` vars (G-S9-1 semantics
  change — unchanged test gap). `EACCES` → `EBUSY` (contention, not
  permission). getlk: `l_pid != getpid` assert; UNLCK⇒dead / else live.
* P4: `lck_setlk_with3retries` bounded (3, ×3 on Android with 21 ms sleeps).
* P2: `check_fstat` thresholds `MIN_PAGESIZE*NUM_METAS` and
  `sizeof(lck_t)+sizeof(reader_slot_t)` are exact minimums for the subsequent
  maps (ties into lck_setup_locked's `maxreaders ≥ 4` gate).

### sysv_ipc_eexist_decide / lck_ftok (lck-posix.c:606-732) — PROVED
Wave-4 attack-6 statement rows: signature extraction consumes `x,y` by 14-bit
semval steps (shift 50 = 64-14 is exact; y exhausted after 4 steps); the
narrow-system multiply-spread is bijective on the low bits (odd constants).
Decision table: foreign `sem_nsems` → flee; unstable ctime >8 attempts → flee;
signature mismatch → flee; match → reclaim.

### dxb_shrink_attempt_allowed — POSIX (lck-posix.c:1109-1112) — PROVED
Constant TRUE (ftruncate works under the mapping; no suspend needed) — the
Windows counterpart carries the logic.

### lck_seize / lck_downgrade / lck_upgrade — Windows (lck-windows.c:499-537, 539-565, 567-605) — PROVED·A(win-reading)
* 9-state FSM statement rows (lck-windows.c:363-392): seize
  `?-? → ?-E(TIMEOUT_LONG) → E-E(SHORT)=TRUE`, on sharing → `S-E(LONG)` →
  unlock UPPER → `S-?(FALSE)` or `?-?(rc)`; non-sharing at E-E step →
  `funlock(UPPER)` + rc. downgrade `E-E → ?-E → S-E(LONG) → S-?` (failure
  leaves the transitive middle state; the open-failure path closes fds and
  dissolves it — bounded). upgrade `S-? → S-E → ?-E → E-E`, contention mapped
  to `MDBX_BUSY` at both steps (transitive state left behind on step-4
  failure, same dissolution argument). All `funlock` failures → `panic_fmt`.
* lck_seize win: EXCLUSIVE ⇒ TRUE (files opened unshareable); lckless ⇒
  shared whole-dxb `LCK_DONTWAIT` (error ⇒ return rc — no lckless-grant,
  consistent with INV-S9-4); TRUE && !RDONLY ⇒ lock-against-without-lck probe:
  exclusive whole-dxb, fail ⇒ `lck_unlock` + rc; then funlock of the probe
  (per-handle ownership, same fd that locked).

### lck_txn_lock / lck_txn_unlock — Windows (lck-windows.c:97-137) — PROVED·A(win-reading)
dontwait ⇒ `TryEnterCriticalSection` else `osal_fastmutex_acquire`;
EXCLUSIVE ⇒ CS-only; else `flock_dxb(DXB_BODY)`; failure ⇒ leave CS, map
`ERROR_LOCK_VIOLATION→MDBX_BUSY` only when dontwait. Owner set only on
success; unlock order = exact reverse (funlock, owner=0, leave CS);
unlock failure ⇒ panic. Single-writer invariant rests on CS + per-handle
byte-range lock (Windows locks are per-handle — both taken through the same
`fd4data`).

### lck_rdt_lock / lck_rdt_unlock — Windows (lck-windows.c:150-187) — PROVED·A(win-reading)
`remap_lock` shared → `lck_event_cs` → lckless/exclusive ⇒ done; else
exclusive UPPER (blocking). Failure releases CS then SRW (reverse order);
unlock = funlock UPPER → Leave CS → release SRW. P6: SRW-shared + CS
serialization is the windows analogue of the POSIX rdt mutex.

### flock_ex / funlock / flock_lck / flock_dxb (lck-windows.c:19-83) — PROVED·A(win-reading)
* P1: asserts pin the contract (timeout>0 ⇒ event && !DONTWAIT).
* P3/P5 overlapped discipline: `ERROR_IO_PENDING` ⇒ timeout wait
  (`TIMEOUT → ERROR_LOCK_VIOLATION` → bailout) or blocking
  `GetOverlappedResult`; bailout always `CancelIo` + blocking
  `GetOverlappedResult` and releases the lock if it was granted before the
  cancellation took effect — the stack-resident OVERLAPPED never outlives the
  I/O. `CancelIo` (not Ex) suffices: same-thread issuance. MPROBE injection
  path returns the injected code without issuing a request (no cancellation
  needed).
* `flock_dxb` picks the overlapped fd when present — the same handle that
  writes (per-handle write-into-locked-region rule).

### lck_unlock (win) / osal_lockfile (win) (lck-windows.c:394-445, 189-191) — PROVED·A(win-reading)
Drain loops `do funlock while(SUCCESS)` until `ERROR_NOT_LOCKED` (Wine
`ERROR_LOCK_VIOLATION` tolerated) over LCK_LOWER, LCK_UPPER, DXB_BODY,
DXB_WHOLE, then the fd4meta meta-region lock of the exclusive fast path
(guarded by `env_exclusive_fastpath()` — lck.h:103-108: EXCLUSIVE&&!RDONLY &&
!Wine && !stuck_meta); `SetLastError(ERROR_SUCCESS)` resets after each drain.
P4: each drain terminates at the first NOT_LOCKED. P5: fd4meta reset in
env_close AFTER the fds (see env_close row) keeps the destroy-time predicate
valid.

### windows_seize_exclusive (lck-windows.c:206-232) — PROVED·A(win-reading)
Gated by `env_exclusive_fastpath`; meta region via fd4meta DONTWAIT → body
via fd4data; body failure ⇒ rollback meta unlock (`panic` if rollback fails)
+ sharing→BUSY. Per-handle rule honored (comment lck.h:109-117).

### internal_seize_lck (lck-windows.c:453-497) — PROVED·A(win-reading)
The FSM core; each `jitter4testing` site is a probe no-op. P5: the
`funlock(UPPER)` at step 8 covers both S-E and ?-E origins; failure ⇒ panic.

### lck_init — Windows (lck-windows.c:607-626) — PROVED·A(win-reading)
`SetFileIoOverlappedRange` privilege probe; every failure mode (token open,
lookup, adjust, non-SUCCESS last error) disables the import (nullptr) and the
token handle is closed on all paths. Pure capability probe — no state change.

### lck_destroy — Windows (lck-windows.c:628-644) — PROVED·A(win-reading)
munmap dxb **before** releasing locks (STATUS_USER_MAPPED_FILE avoidance),
munmap lck, then `synced && !neighbor && lck_upgrade(dontwait)==SUCCESS` ⇒
`fsetsize(0)` (fails benignly if others still map), then `lck_unlock`. The
fork/pid guards of the POSIX version are unnecessary here (no ipc objects);
wave-4 contract row reproduced.

### lck_rpid_set / clear / lck_rpid_check — Windows (lck-windows.c:649-696) — PROVED·A(win-reading)
set/clear are no-ops (handle existence is the indicative lock). check:
`OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(0)`; verdict table complete:
`ERROR_INVALID_PARAMETER`⇒dead, `WAIT_OBJECT_0`⇒dead, `WAIT_TIMEOUT`⇒live,
`ERROR_ACCESS_DENIED`⇒live (CSRSS et al.), `WAIT_FAILED`/other ⇒ error.
INV-S9-3 Windows half — handle liveness plays the pid-byte role.

### suspend_and_append / osal_suspend_threads_before_remap / osal_resume_threads_after_remap (lck-windows.c:234-357) — PROVED·A(win-reading)
* P2/P3 growth arithmetic: new capacity `limit*2`; allocation
  `header + HANDLE*(limit*2 - N)` where the header itself holds N handles ⇒
  exactly `limit*2` slots; stack-origin array copied by value on first growth
  (`limit == N`), heap array grown in place; ENOMEM propagates without state
  change.
* P1 filters (lck path): `pid != env->pid`, tid 0 or ≥ `MDBX_TID_TXN_OUSTED`,
  self tid, write-txn owner — all skipped; scan bound `rdt_count` (L2 clamp).
  lckless path asserts `EXCLUSIVE|RDONLY` and walks the Toolhelp snapshot of
  `env->pid` minus self; requires the trailing `Thread32Next` error to be
  `ERROR_NO_MORE_FILES`.
* P5: any failure ⇒ `osal_resume_threads_after_remap(*array)` (resumes all
  suspended, closes handles, `count=0`) and returns the error — no suspended
  thread is ever left on a failed open/resize. `SuspendThread(-1)` with
  `ERROR_ACCESS_DENIED` (Win10 UCRT) or a non-STILL_ACTIVE exit code is
  treated as "thread already gone" — handle closed, not appended (no leak).
* Resume: first error wins, all handles resumed+closed regardless.
* MPROBE `suspend_open_thread_error` evaluated before any suspension (probe
  bus lock hazard documented at the call site).

### dxb_shrink_attempt_allowed — Windows (lck-windows.c:701-736) — PROVED·A(win-reading)
Rate limit `now - shrink_last_attempt < interval` (monotonic clock, no wrap);
foreign-live-pid scan clamped by `rdt_count`, verdict via `lck_rpid_check`
(stale slots filtered) ⇒ skip the doomed shrink attempt.

---

## 2. `src/rthc.c`

### rthc_sibling_find (rthc.c:23-35) — PROVED
rthc_lock-held scan by lck-fileid; skips self and fd-less entries; fileid
set-ness asserted (published-before-descriptor invariant, lck.c:210-211).

### rthc_lock / rthc_unlock (rthc.c:147-161) — PROVED
POSIX `ENSURE`ed mutex ops; Windows CRITICAL_SECTION. The mutex is
re-initialized (not unlocked) in the fork child — see rthc_afterfork.

### rthc_signature / rthc_read / rthc_compare_and_clean (rthc.c:48-86) — PROVED
P2: `salt<<8|kind` little-endian / `kind<<56|salt>>8` big-endian — the kind
byte sits in the 64-bit CAS word's stable byte on both endiannesses; 32-bit
CAS paths CAS the kind-carrying half exactly. `#error FIXME` for mixed
byte-order is compile-time only (INV-S9-6 class).

### rthc_atexit (rthc.c:88-129) — PROVED
Weak-symbol dispatch `__cxa_thread_atexit` / `_tlv_atexit`; rc==ENOSYS drives
the pthreads-fallback in thread_rthc_set. Feature macros resolved from glibc
2.18/BIONIC/libcxx probes — build-time, all branches return a defined rc.

### thread_rthc_set (rthc.c:175-195) — PROVED
REGISTERED/COUNTED state machine (P6): a thread registers at most once
(state guard against both signatures); cxa path stores REGISTERED and passes
`&rthc_thread_state` with a DSO anchor; fallback path: `pthread_setspecific(rthc_key)`
+ COUNTED + `atomic_add32(rthc_pending)` with the `count_before < INT_MAX`
bound (P2). Repeat sets skip registration. The final `pthread_setspecific(key,
value)` is the caller's key regardless. The `rthc_pending` increment is not
under `rthc_lock` — benign: the dtor's missed-wake window costs one 100 ms
wait, never a lost cleanup (A-V7C-4).

### rthc_thread_dtor (rthc.c:198-270) — PROVED
Statement rows (wave-4 attack 8 held):
1. `rthc_lock()` first — the whole body is registry-consistent.
2. Per-env loop: skip foreign pid, lck-less (`!lck_mmap.lck`), non-ENV_TXKEY.
3. Slot bounds `thread_rthc_get(me_txkey)` ∈ `[&rdt[0], &rdt[max_readers])`
   (capacity bound, stronger than L2 — INV-S9-2).
4. `pthread_setspecific(me_txkey, nullptr)` failure ⇒ tsd-key deletion race
   with `mdbx_env_close` ⇒ `continue` (the key is gone; no slot to clear).
5. `pid == current_pid` guard + `atomic_cas32(pid, current_pid, 0)` — the CAS
   closes the bind-race (a concurrent `mvcc_bind_slot` can only claim the
   zeroed slot and re-poison; a dying thread's CAS then simply fails).
6. `rdt_refresh_flag = true` only when something was cleared.
7. POSIX tail: self-state consume — REGISTERED CAS-cleared silently, COUNTED
   CAS-cleared + `rthc_pending--` (`> 0` ensured), unknown ⇒ WARNING (no
   action). `rthc_pending == 0` ⇒ `pthread_cond_broadcast` (wake the exiting
   process's rthc_dtor).
8. P5/DSO-unload: final unlock via a non-tailcall-safe explicit
   `pthread_mutex_unlock` with the comment explaining the required `jmp`
   (code section may be unloaded immediately after).

### rthc_neighbor_check (rthc.c:284-316) — PROVED
P1 decision table complete and deadlock-free-by-construction: same dxb
fileid required to interact at all; both-have-lck ⇒ siblings iff same lck
fileid (`*out_neighbor`), otherwise BUSY (same dxb via two lck files = no
mutual exclusion); only-neighbor-has-lck or we-are-EXCLUSIVE ⇒ BUSY;
both lck-less ⇒ no conflict. Consumer fence at entry pairs with the producers'
publication (lck.c:210). Called under `rthc_lock()` at all three sites
(lck.c:141/217, and via rthc sibling paths).

### rthc_register (rthc.c:318-357) — PROVED
* P1: duplicate registration of the same env ⇒ MDBX_PANIC (dup-registration
  PANIC path; reachable only via env-lifecycle misuse since env_close
  unregisters).
* P2/P3 capacity: doubling `rthc_limit*2` from 16; `memcpy` of the static
  prefix on first growth; overflow needs 2^31 live envs — unreachable.
  `me_txkey` created only when `!NOSTICKYTHREADS` (ENV_TXKEY set on success);
  key-create failure ⇒ bailout with no table entry (key already zeroed).
* P5: on any error nothing is added to the table (`goto bailout` before the
  slot write).
* See F-V7C-1 for the post-`mdbx_fini` reuse gap (not a defect within the
  supported lifecycle).

### rthc_drown (rthc.c:359-390) — PROVED
P5 completeness: guard `fd != INVALID && current_pid == env->pid` (fork:
skip — INV-S9-5); sibling found ⇒ skip slot cleanup (neighbor still uses the
region); else clear all slots owned by `current_pid` over the CAPACITY range
`[rdt[0], rdt[max_readers])` via CAS + refresh flag, `lck_rpid_clear` when a
registered pid-byte exists; finally `lck_destroy(env, neighbor, current_pid)`
and `env->pid = 0`. rc = first error of (rpid_clear, lck_destroy).

### rthc_remove (rthc.c:392-419) — PROVED
Drown only when `env->pid` non-zero (not-yet-opened env). Swap-with-last
removal; empty dynamic table ⇒ restore the static table with a full fence
before `osal_free(tmp)` (comment: publish before giving away). Under
`rthc_lock()` by both callers (env_close:570-572).

### rthc_beforefork / rthc_fork_trylock / rthc_afterfork_parent (rthc.c:424-486) — PROVED·A
* Try-lock-only ladder `rthc_mutex → per-env dbi_lock → remap_lock →
  debug_lock`; contention ⇒ release everything and retry; `debug_lock` waited
  for while holding nothing; env-mutex contention backs off up to 1 ms
  (A-V7C-3: no upper bound — fork blocks until the mutexes are free; that is
  the designed semantics: never wait while holding rthc_mutex, avoiding the
  dtor/logger join deadlocks per the comment).
* Unlock order of afterfork_parent is the exact reverse (debug, then envs
  high-to-low, then rthc).

### rthc_afterfork (rthc.c:488-524) — PROVED
Child reinit list (P5 completeness, INV-S9-5): `debug_lock`, `rthc_mutex`,
`rthc_cond` re-initialized (recorded under a vanished tid); per-env
`dbi_lock`/`remap_lock` re-init; `basal_txn->owner = env->txn ? self : 0`
(handed to the surviving thread for resurrect-abort); both mmaps munmapped
(+valgrind discard); `env->lck = lckless_stub`; `rthc_drown` (pid-guarded:
closes fds, never destroys the parent's ipc objects); dynamic table freed,
`count=0`, static table restored, `rthc_pending.weak = 0`.
Cross-checked non-duplicates: `registered_reader_pid` reset lives in
`mdbx_env_resurrect_after_fork` (api-env.c:594), not here — and env_close's
ENV_TXKEY branch deletes `me_txkey` at resurrect; the un-resurrected child
simply never touches it again. No gap.

### rthc_ctor (rthc.c:527-536) — PROVED
POSIX: `pthread_atfork(handlers)` + `rthc_key` creation (dtor =
`rthc_thread_dtor`); Windows: CRITICAL_SECTION init. ENSURE-abort on failure
(constructor cannot report errors). Called from `mdbx_init` after
`osal_ctor` (globals.sys_pagesize ready for later users).

### rthc_dtor (rthc.c:538-623) — PROVED / F-V7C-1
* Self-state consume mirrors rthc_thread_dtor (REGISTERED/COUNTED/wrong),
  then the 100 ms deadline wait loop: `abstime = now + 100 ms` (+600 s in
  debug builds); P2 nsec normalization exact; P4: `pthread_cond_timedwait`
  EINTR-only-retry against an ABSOLUTE deadline ⇒ terminates (a wake with
  pending>0 re-enters with a past deadline ⇒ ETIMEDOUT ⇒ break).
  `thread_key_delete(rthc_key)` (with the 3-yield glibc workaround) — after
  this, fallback-key users can no longer register (process exiting).
* Per-env exit cleanup: skip foreign pid / lck-less / MAP_FAILED / non-TXKEY
  (redundant second `!lck_mmap.lck` in the guard is harmless); `flags -=
  ENV_TXKEY` (guaranteed set by the preceding check — no underflow),
  `thread_key_delete(me_txkey)`, capacity-range slot clear + refresh flag.
  No double-delete with env_close: a closed env was already unregistered
  from the table.
* Teardown: `rthc_limit = rthc_count = 0`, free dynamic, `rthc_table =
  nullptr`, unlock, then `workaround_glibc_bug21031()` (3 sched_yield) after
  the lock is released.
* **F-V7C-1 (low, hardening):** the post-dtor state disarms the registry
  (`table=nullptr, limit=0`; Windows also `DeleteCriticalSection`). An
  application calling `mdbx_env_open` after `mdbx_fini` (atexit-order
  violation / DSO-unload misuse) is undefended: `rthc_register` sees
  `count==limit (0==0)`, `realloc(nullptr, 0)` may succeed leaving
  `rthc_limit == 0`, writes `table[0]`, and a SECOND env then writes
  `table[1]` past a zero-size heap block (heap overflow); on Windows
  `EnterCriticalSection` on a deleted CS (UB). Unreachable in any supported
  lifecycle; fix form: a `finalized` flag returning `MDBX_EPERM`/panic, or
  floor the restored `rthc_limit` at `RTHC_INITIAL_LIMIT`. Counterexample
  trace: `exit → mdbx_global_destructor → mdbx_fini → rthc_dtor` then
  `atexit-callback → mdbx_env_open ×2` (POSIX, static link).

### workaround_glibc_bug21031 (rthc.c:131-144) — PROVED
P4: exactly 3 sched_yield — bounded by design (the comment's "chance, not
guarantee"); P5 no state. Called from `thread_key_delete` (rthc.h) and the
rthc_dtor tail.

### thread_key_create / thread_key_delete (rthc.c:163-173, rthc.h:31-40) — PROVED
Windows TlsAlloc/TlsFree with ENSURE; POSIX delete + the 3-yield workaround
inside the caller's locking context (benign short hold).

---

## 3. `src/mvcc-readers.c` — cross-check only (wave 4 ledgered per-function)

Cross-checked against `doc-additions-v8-b.md` §1/§2 and the KB facts; **no
discrepancies found, nothing new:**

* `mvcc_bind_slot` (6-73): INV-S9-1 order verified verbatim at
  60→61→62-63→64→65; the slot-search loop terminates (each continuing
  iteration requires `cleanup_dead` to have returned RESULT_TRUE or cleaned
  ≥1 slot — finite slots). Errors unlock the reader mutex on every path
  before returning; `thread_rthc_set` happens after unlock (short lock hold).
* `mvcc_snapshot_oldest_ro` (77-106): refresh-flag double-check fast path;
  the ABA window (flag restored to 'None' between the two loads) yields a
  stale-but-lower-bound cache value — safe under PAIR-N10 (consistent with
  wave 4 §5).
* `mvcc_snapshot_oldest_rw` (108-204): distrust reset, 4-pass `may_lag` bound
  (#325), per-slot retry re-reads, monotonic cache update asserts — matches
  wave-4 rows.
* `mvcc_snapshot_largest` / `mvcc_largest_this` (206-247): stable-pair retry
  loops; this-process filter by `registered_reader_pid`; the
  `cached_oldest <= snap_txnid <= MAX_TXNID` gate.
* `pid_insert` (249-280): binary search + shift insert; cursor ≤ n always
  (in-bounds reads), `list[0]` ≤ distinct pids ≤ snap_nreaders, allocation
  `snap_nreaders+1` ⇒ writes stay in bounds. PROVED (fills the P2/P3 note
  wave 4 did not spell out).
* `mvcc_cleanup_dead` (282-376): INV-S9-3 re-verified statement-level incl.
  the unlocked path's re-lock + slot-reuse recheck (340/343), the inner
  `ii` loop clamped by `snap_nreaders`, timestamp update only on non-error,
  `rdt_locked<0` unlock, malloc/free pairing. V-050 asymmetry unchanged.
* `mvcc_kick_laggards` (378-512): `(intptr_t)pid <= 0` guard present at
  :461; parked-oust CAS + `safe64_reset_compare` (INV-T8); the rc>1 full
  clear poisons txnid FIRST, then tid, then pid — the exact mirror of the
  bind order (poison-before-publish discipline preserved on clear);
  `retry < INT_MAX` (V-036); final eof-of-loop callback with `retry<0`.

---

## 4. `src/global.c`

### mdbx_module_handler / DllMain (global.c:42-116) — N-A(on Linux) / PROVED·A(win-reading)
PROCESS_ATTACH ⇒ windows_import + mdbx_init; DETACH ⇒ mdbx_fini;
THREAD_DETACH ⇒ `rthc_thread_dtor(module)` (the rthc argument is unused on
Windows beyond TRACE — the TLS-callback signature). The `.CRT$XLB` anchor and
`_tls_used` linkage scaffolding is P7/build plumbing, no runtime statements.

### probe_for_WSL (global.c:124-136) — PROVED
P1: `WSL[3] ∈ ['2','9']` ⇒ that version; any WSL/Microsoft marker ⇒ WSL1
iff kernel < 4.19 else WSL2 (false-negative ⇒ data-loss requirement honored
by the conservative default). P2: digit range check precedes subtraction.

### mdbx_global_constructor (global.c:148-185) — PROVED
Kernel-version parse loop (P4): `while (*p && i < 4)`; digit runs consumed by
`strtol` (advances past overflow too), non-digits by `++p`; a "0" component
still advances `i` — terminates at NUL. Component clamp 255 before the
24-8i shift — no field overlap. WSL detection as above; then `mdbx_init()`.

### mdbx_global_destructor (global.c:188-194) — PROVED
`mdbx_fini` + optional gprof `_mcleanup`. Destructor-order caveat ⇒ F-V7C-1.

### getenv_bool (global.c:202-215) — PROVED
P1 value grammar complete: empty ⇒ ON; yes/on/true/1 ⇒ ON; no/off/false/0 ⇒
OFF; anything else (and absence) ⇒ default. `osal_getenv_singlethreaded` is
correct here (constructor, pre-threads).

### assume_ram_pages (global.c:217-235) — PROVED·A
P2: fallback `MAX_MAPSIZE32/3 >> ln2` (~174k pages at 4 KiB) — `(total+avail)`
cannot overflow (intptr_t page counts ≪ 2^52); the 32-bit clamp
`SIZE_MAX >> ln2` precedes the ASAN/Valgrind halving. A-V7C-5: assumes
`mdbx_get_sysraminfo` reports a positive total on any working system
(a zero report would only drive the limits to 0 — degraded, not unsafe).

### mmap_limit (global.c:237-254) — PROVED·A
`STATIC_ASSERT(MAX_MAPSIZE < INTPTR_MAX)`; ASAN 16 TiB and Valgrind 512 MiB/
32 GiB clamps; final `min(limit, assume_ram_pages << ln2)` — the shift is
overflow-free because `assume_ram_pages ≤ SIZE_MAX>>ln2` (32-bit clamp) or is
bounded by real-RAM pages (64-bit). A-V7C-5 as above (`ASSERT(assume_ram_pages
> 0)` is a checking-build guard over the same assumption).

### reasonable_db_maxsize (global.c:256-277) — PROVED
P2: `assume_ram_pages * 207 >> 7` (golden ratio) — ≤ 2^57 on any real system,
no overflow; the `limit = mmap_limit >> (ln2 + (WORDBITS<64))` shift is
well-defined for any limit (result 0 degrades safely); the up-rounding branch
is bounded (`ceil > mmap_limit/4` forces the down branch), so the final
`ASSERT(result <= mmap_limit)` holds by construction.
P4 rounding loop: 64-bit `unit <<= 5` wraps 2^60→0 ⇒ loop condition ends it;
32-bit the sequence 2^20→2^25→2^30→2^3→… cycles, but the break fires at the
latest at `unit == 2^30` (floor==0 ⇒ `result-floor == result ≥ result>>4`,
and `ceil(2^30) > mmap_limit/4` since `MAX_MAPSIZE32 ≤ 0x7f000000`) —
terminates on both word sizes (no infinite micro-rounding).

### mdbx_init (global.c:279-304) — PROVED
Order: runtime_flags (getenv_bool ×8, flag-disjunction) → loglevel →
debug_lock init → `osal_ctor` (sys_pagesize etc., power-of-2 asserted) →
`rthc_ctor` → CHECKING-only `troika_verify_fsm` + `pv2pages_verify` →
`assume_ram_pages` → `mmap_limit` → `reasonable_db_maxsize` (each consumes
the previous — dependency order exact). ENSURE-abort on failure (constructor).
pv2pages_verify: one-line cross-check of the wave-2 machine check — the
exhaustive 65536-entry `pv2pages∘pages2pv∘pv2pages == pv2pages` identity loop
(cogs.c:59-75), run at every CHECKING startup.

### mdbx_fini (global.c:306-314) — PROVED
`rthc_dtor(current_pid)` → `osal_dtor` → debug_lock destroy. P5: process-exit
only; post-fini reuse gap ⇒ F-V7C-1.

### mdbx_build / __asan_default_options (global.c:321-603) — N-A
Const ABI data and sanitizer defaults; no P1–P6 statements (P7 string
plumbing, verified by the amalgamated builds).

---

## 5. `src/env.c`

### env_owned_wrtxn (env.c:6-10) — PROVED
The "txn-owned ops" predicate: owner==self ⇒ `env->txn ?: basal_txn`;
used by env_sync/env_info/stat-class readers to decide lock-free tap vs
txn-owned path. P1: complete and total (null when not owned).

### env_page_auxbuffer (env.c:12-21) — PROVED
P3: buffer `ps*NUM_METAS` (NUM_METAS=3, layout-dxb.h:31) vs touched
`ps*2 + ps = ps*3` — exact fit; alloc failure propagates; idempotent via the
`env->page_auxbuf` guard.

### env_setup_pagesize (env.c:23-58) — PROVED
P1 guards: power-of-2, [MIN,MAX], not-yet-auxbuf && ps-change (re-setup
refused); STATIC_ASSERTs pin the per-pagesize derivable bounds.
P2: `maxgc_ov1page > 42 && < PAGELIST_LIMIT/4`; node-max gates (parity,
min payload) — wave-2 ladder re-verified statement-level; `pgno2bytes==pagesize`
and `bytes2pgno(2ps)==2` self-checks; thresholds recalculated last.

### env_sync (env.c:60-208) — PROVED·A
* P1: RDONLY ⇒ EACCESS; the `(flags & (ENV_FATAL_ERROR|ENV_ACTIVE)) !=
  ENV_ACTIVE` ALL-bits gate ⇒ PANIC (fatal) / EPERM (not active) — the L8/
  wave-5 ENV_FATAL contract, statement-level.
* P6 troika choice: `txn_owned||should_unlock ⇒ basal troika` (stable under
  our own wrt_lock) else `meta_tap` (lock-free multi-version tap, validated
  downstream); `unsynced_pages` read once and re-read after taking the lock.
* P4 `retry_presync` loop (env.c:112-163): each iteration requires another
  writer to have synced `> presync_threshold` MORE pages within our
  unlock→relock window (strict progress on a monotonically-changing counter);
  A-V7C-6: termination assumes the writers' sustained commit rate does not
  perpetually outrun our presync+relock cycle — the writer cannot sync while
  we hold the wrt_lock, so each window is short; a hostile high-rate writer
  could prolong the loop while doing real work (live-lock under permanent
  saturation, no correctness impact). `nonblock` maps contention to
  MDBX_BUSY/`dont_wait` errors — bounded in that mode.
* P5: presync failure (before any lock is held) ⇒ `ENV_FATAL_ERROR` + direct
  return — the OS-may-drop-pages comment forbids retry (signing a steady over
  lost pages); `meta_sync` failure after `dxb_sync_locked` ⇒ ENV_FATAL + rc
  propagates through the bailout unlock (`should_unlock` covered).
* Coherency-#269 placement (cross-ref coherency.c): env_sync does NOT run the
  yield-timeout loop — instead the volatile meta is copied,
  `osal_fence_consumer`, and `memcmp`-re-read (env.c:183-185) before
  `dxb_sync_locked`; a torn/volatile head ⇒ `MDBX_PROBLEM`. The
  timeout-yield loops live on the read side (`coherency_fetch_head`/
  `coherency_check_written`/page-iov) — placement is deliberate: the sync
  path holds the writer mutex and must not spin; it refuses instead.
  (`coherency_timeout` itself: 100 ms deadline, EINTR-free, yields — PROVED;
  the incoherence counter is INT32_MAX-saturating, no overflow.)
* txn-owned path: `basal_txn->txnid = head.txnid`, `txn_gc_detent`,
  `txn_shrink_allowed`; asserts pin `!env->txn && !nested` on the should_unlock
  path and `!(flags & txn_shrink_allowed)` on the owned path.

### env_rigorous_envmode_diff (env.c:229-234) — PROVED
Flag-domain XOR masked by the rigorous set (`SAFE_NOSYNC|UTTERLY_NOSYNC` plus
WRITEMAP only for lazy modes) — the multi-process WRITEMAP/durability
compatibility rule documented by the preceding comment block.

### env_open (env.c:236-557) — PROVED·A
* P5 ladder statement rows (wave-2 verified, re-checked): fileid-by-path
  formed before any open (POSIX, env.c:326-332); dxb open ⇒ fileid-by-fd
  re-verify (replace warning); SysV ftok before lck; parking-lot fseek;
  Windows event/overlapped-handle failures return early — every early return
  is reclaimed by the caller's `bailout: env_close(env,false)`
  (api-env.c:558-562), which closes events/fds and drowns via rthc_remove
  (env->pid set at entry, env.c:324). No leak on any partial-open path.
* P1 `envmode` adoption: the CAS loop (env.c:472-489) replaces an RDONLY
  envmode with ours; ACCEDE picks up mode-flags (lazy decides WRITEMAP
  negotiability); `env_rigorous_envmode_diff` ⇒ INCOMPATIBLE. A-V7C-7: the
  CAS loop has no starvation bound (standard lock-free retry; the contended
  word is written once per opener).
* stuck_meta ⇒ BUSY unless exclusive initializer (recovery exclusivity,
  env.c:523-527); initializer publishes `envmode/meta_sync_txnid/
  readers_check_timestamp` (env.c:531-535) BEFORE `lck_downgrade` — matches
  wave-4 attack 6 (SETALL under exclusive, downgrade after).
* Cooperative path runs `mvcc_cleanup_dead(false)` once at open (env.c:543).
* The FIXME at env.c:485 (re-check DB size after RDONLY-first-initializer)
  remains a documented open question — unchanged, no defect.

### env_close (env.c:559-667) — PROVED
Statement rows (P5/P6 ordering — the "env registry vs in-flight txns" item):
1. `flags` snapshot; `ENV_INTERNAL_FLAGS` cleared (ENV_TXKEY included).
2. `ENV_TXKEY` (checked on the SNAPSHOT — the mask already cleared it) ⇒
   `thread_key_delete(me_txkey)`; this is the deletion that
   `rthc_thread_dtor`'s setspecific-failure path tolerates (wave-4 attack 8).
3. `munlock_all` while `env->lck` still valid.
4. `rthc_remove` under `rthc_lock` ⇒ drown (slot clear, pid-byte clear,
   `lck_destroy`: neighbor restore + ipc drown + ftruncate) — in-flight
   RO txns of this process get their slots cleared here; later use surfaces
   `MDBX_BAD_RSLOT` (fail-safe, documented contract).
5. defer_free list freed; ioring destroyed (non-RDONLY).
6. `env->lck = nullptr`; lck mmap and dxb mmap munmapped (+valgrind).
7. Windows: events closed; `fd4meta = INVALID` AFTER dsync/lazy/lck fds are
   closed (the comment at env.c:624-629 — the failed-open-retry dangling
   handle fix; lck_unlock's fd4meta use is only reachable before this point).
8. Non-resurrect frees kvs/auxbuf/dbi_seqs/dbs_flags/pathname/basal_txn;
   `stuck_meta = -1`; rc = rthc_remove's (caller escalates to PANIC).

### mdbx_env_close_ex / mdbx_env_resurrect_after_fork / mdbx_env_sync_ex (api-env.c:566-695, 1371-1377) — PROVED (cross-ref rows)
* close_ex: PID check ⇒ ENV_FATAL (fork legitimization), owner-conflict ⇒
  BUSY before the signature CAS; sync (POSIX skips deleted files via nlink);
  conditional `lck_txn_unlock` (CHECKOWNER-aware); `env_close` rc ⇒ PANIC;
  mutex destroys in reverse init order; stub wrt ipclock destroyed;
  shadow_reserve pool drained; env freed.
* resurrect: signature CAS (re-entrancy guard), inherited txn abort via
  `txn_abort_after_resurrect`, `registered_reader_pid = 0`,
  `env_close(resurrect=true)` then `env_open` (EXCLUSIVE ⇒ BUSY) — the
  INV-S9-5 consumer, statement-level re-checked.

---

## 6. Findings

| F-id | Where | Суть | Severity | Machine check |
|---|---|---|---|---|
| F-V7C-1 | rthc.c:538-623 (rthc_dtor) + rthc.c:329-340 (rthc_register) + global.c:306-314 | **Post-`mdbx_fini` reuse is undefended:** the dtor leaves `rthc_table=nullptr, rthc_limit=0` and (Windows) deletes `rthc_critical_section`; a later `mdbx_env_open` (atexit-order violation / DSO-unload misuse) passes the `count==limit (0==0)` gate, `realloc(nullptr,0)` may succeed leaving `rhtc_limit==0` (rthc_limit *= 2 keeps 0), and a SECOND registration writes `rthc_table[1]` past a zero-size heap block — heap overflow; Windows: `EnterCriticalSection` on a deleted CS (UB). Unreachable in any supported lifecycle; no diagnostic today. Fix form: finalized-flag ⇒ clean error, or floor `rthc_limit` at `RTHC_INITIAL_LIMIT` when restoring | low | code-path trace (see §2 rthc_dtor row) |

Cross-checks that closed without new items: mvcc-readers.c (§3 — wave 4
verdicts hold verbatim, incl. the `pid <= 0` HSR guard at :461 and the
clear-path poison order), V-049..V-052 unchanged, the half-initialized lck
region recovery (lck.c:92-103 + lck-posix.c:890 bailout) — safe by the
missing format stamp, the env_open partial-open ladder — reclaimed by the
caller's bailout env_close.

Assumptions register (PROVED·A obligations): A-V7C-1 `sizeof(lck_t) <
sys_pagesize` on every supported platform (layout is cacheline-scale; a
65536-byte sys page only widens the margin); A-V7C-2 hostile external file
truncation is outside the trust model (V-052 class); A-V7C-3 fork preparation
blocks indefinitely on a permanently-held env mutex (designed); A-V7C-4
`rthc_pending` add outside `rthc_lock` — missed-wake window costs one bounded
100 ms wait; A-V7C-5 `mdbx_get_sysraminfo` positive total on working systems;
A-V7C-6 sustained-writer live-lock in env_sync's retry_presync is transient
in practice; A-V7C-7 lock-free CAS loops in env_open/envmode have no
starvation bound.

## 7. New lemmas (proposed for libmdbx-invariants.md, S9 area continuation)

* **LEM-V7C-1 (half-init lck refusal):** any `lck_init` failure after the
  magic write leaves the region with `magic ✓, os_and_format == 0`; every
  subsequent opener returns MDBX_VERSION_MISMATCH (lck.c:99-103) and only an
  exclusive opener's memset recovers it — no process can ever attach to a
  half-initialized shared world.
* **LEM-V7C-2 (clear-path poison order):** every reader-slot invalidation
  writes txnid-poison (or the CAS/clear) BEFORE the pid store, mirroring
  INV-S9-1's bind order — mvcc_bind_slot, rthc_thread_dtor (pid CAS with
  owner re-check), mvcc_cleanup_dead, kick_laggards rc>1
  (mvcc-readers.c:488-493) — so scans never observe pid≠0 with a stale valid
  txnid from OUR clear paths.
* **LEM-V7C-3 (reasonable_db_maxsize termination):** the human-granulation
  rounding loop terminates on 32-bit (break ≤ unit 2^30) and 64-bit (unit
  wraps to 0) and its result never exceeds `globals.mmap_limit`.

## 8. KB keys written (shared-graph-memory)

* `fact:verification:coord-perfunction` (normalized from
  `coord-s9-perfunction` by the Store) — this tranche's consolidated
  lemmas/contracts (lck/rthc/windows-FSM/env_sync/close ordering).
* `bug:global:rthc-postfini-reuse` — F-V7C-1 (importance 0.4).
* links: fact→`fn:lck_setup`, `fn:rthc_register`, `fn:rthc_dtor`,
  `fn:env_sync`, `fn:env_close`, `fn:internal_seize_lck`; bug→`fn:rthc_register`.
