# Verification wave 8, stream B (adversarial, S9 coordination) — proposed doc additions

Scope: `src/lck.c`, `src/lck-posix.c`, `src/lck-windows.c`, `src/rthc.c`,
`src/global.c`, `src/mvcc-readers.c` (+ their direct support read for
consumers: `src/lck.h`, `src/layout-lck.h`, `src/atomics-ops.h` (safe64),
`src/txn-ro.c` (ro_slot_*), `src/env.c`/`src/api-env.c` (close/resurrect
ordering), `src/osal/posix/osal-{init,bootid,fs}.c`, `src/options.h` (flavor
selection)).

All verdicts follow the LEDGER protocol (`PROVED` / `PROVED·A` / `REFUTED` /
`SUSPECT`). No REFUTED defect was found in S9: four findings V-049…V-052
(doc/hardening/by-model, all low). Windows items are **verified by code
reading only** (no Windows CI in this wave) — every such verdict is labeled
`PROVED·A(win-reading)`.

Reused prior results (not re-proven): L2 (`rdt_count` clamp, internals.h:747),
PAIR-N10 (cached-oldest distrust + kick termination), V-034 (forged
cached-oldest RO fatality), INV-T8 (unpark base-ABA), L8 (ENV_FATAL + the
V-025 exception), V-036 (hsr INT_MAX loop bound), safe64 weak/cas split
(atomics-ops.h:357-445), glibc bug 21031 workaround (rthc.c:131).

---

## 1. New invariants for `docs/engineering/libmdbx-invariants.md` (S9 area)

### INV-S9-1 — reader-slot publication order (bind/claim protocol)

`mvcc_bind_slot` (mvcc-readers.c:54-66), holding the reader-table mutex:
(1) `slot->pid = 0` (idempotent reclaim); (2) `safe64_reset(&slot->txnid, true)`
— poisons the txnid (`>= SAFE64_INVALID_THRESHOLD`) **before** any visibility
of the slot; (3) `rdt_length` high-water bump (only when the claimed slot is
beyond the old count); (4) `tid`; (5) `slot->pid = env->pid` publish.
The live txnid is written later by the owner in `ro_seize`
(safe64_write after the used/retired stores + acquire fence).
Consequences verified against all three attack angles:
* a crash at **any** point of the sequence leaves a slot that is either
  `pid == 0` (invisible to every reader scan, reclaimable — leak-free) or
  `pid != 0 && txnid poisoned` (scans skip the value: `>= threshold` never
  lowers `oldest`; other processes reclaim it via `lck_rpid_check` after the
  process dies because the pid-byte lock died with the process);
* a stale **valid** txnid of a dead reader is never visible with a live pid:
  the poison (step 2) always precedes the pid publish (step 5), so the
  two-phase publish closes the stale-txnid window;
* readers that are mid-`ro_seize` cannot make a scanner pair old-txnid with
  new-used: `ro_seize` stores `snapshot_pages_used`/`snapshot_pages_retired`
  **before** the fence and the txnid (`txn-ro.c:161-168`), so a scanner can
  only over-pair *forward* (`new-retired, old-txnid`), which never moves
  `oldest` up nor `largest` down (shrink-safe direction).

### INV-S9-2 — `rdt_length` has a single writer site and every reader is clamped

The occupancy counter `lck->rdt_length` lives in the externally-writable lock
mapping. Writers: exactly one site — `mvcc_bind_slot` (mvcc-readers.c:63),
under the reader-table mutex, therefore serialized across processes/threads;
the plain `.weak = ++nreaders` store is a 32-bit aligned store (no tearing on
any supported ISA) and `nreaders+1 ≤ max_readers` is guaranteed by the
`slot < env->max_readers` gate. Readers: every loop/allocation bound goes
through `rdt_count()` (the L2 clamp — internals.h:747, comment
internals.h:736-746): mvcc-bind, snapshot_oldest_ro/rw, snapshot_largest,
largest_this, cleanup_dead (both loops share the same `snap_nreaders`
snapshot), kick_laggards, windows `osal_suspend_threads_before_remap` and
`dxb_shrink_attempt_allowed`. The rthc paths use the stronger capacity bound
`&rdt[env->max_readers]` (rthc.c:217, 367, 592). The only remaining direct
read is a log-only display in chk.c:491. Therefore a hostile
`rdt_length = 32767` yields bounded scans and a `READERS_FULL`-class liveness
outcome, never OOB.

### INV-S9-3 — pid-byte discipline: OS pid reuse degrades liveness, never correctness

A reader slot with `pid != 0` was always published by a process that holds the
indicative lock (POSIX: one-byte `F_WRLCK` at offset `pid` of the lck file,
lck-posix.c:187-194, placed before the first bind and held while the env's lck
fd is open; Windows: `OpenProcess`+`WaitForSingleObject(0)`,
lck-windows.c:665-696). Therefore:
* a **live** registered reader is never mistaken for dead (its byte/handle is
  held) — no premature slot clear, no MVCC breach through cleanup;
* OS pid reuse can only produce **false-live** (a reused pid belonging to a
  new libmdbx reader of the same DB keeps the dead process's stale slots
  alive): stale slots then pin the oldest txnid until that process exits —
  a GC-retention leak (safe direction), never a wrong-outcome reclaim;
* `mvcc_cleanup_dead` clears `pid == stale` slots only after a *dead* verdict
  plus (in the unlocked path) a re-check under the reader-table mutex with a
  slot-reuse guard (mvcc-readers.c:325-351). The residual TOCTOU window
  (pid reused and re-bound by a new live process between the dead verdict and
  the clear) surfaces as `MDBX_BAD_RSLOT` on the new process's readers —
  fail-safe, no corruption.

### INV-S9-4 — lckless mode has no reader table and no writer (the pair that makes it safe)

`ro_slot_alloc` skips binding entirely when `!env->lck_mmap.lck`
(txn-ro.c:67-68); `mvcc_bind_slot` asserts `env->lck_mmap.lck` and is never
reached in lckless mode; `mvcc_snapshot_oldest_rw` then scans zero slots
(`rdt_count` of the zeroed `lckless_placeholder`) and computes
`oldest == steady`. This is sound **because** lckless and "a writer exists"
are mutually exclusive:
* on POSIX the only lckless entry is `osal_check_fs_rdonly() == ST_RDONLY`
  (osal-fs.c:19/25): a filesystem **mounted** read-only, where no writer
  process can exist at all. A writable filesystem hard-fails `env_open` for
  every lck-open error (the `case MDBX_EACCESS/EPERM` gate
  `F_ISSET(env->flags, MDBX_RDONLY | MDBX_EXCLUSIVE)` at lck.c:167 uses
  ALL-bits semantics, utils.h:7 — EXCLUSIVE alone does not qualify); an
  EXCLUSIVE env on a read-only filesystem also fails (EROFS → default).
  Machine-checked: `repro/v8b-lckless-reader-mvcc.c` E2/E3;
* in-process writers on a `MDBX_RDONLY` env are rejected (`api-txn.c:258`).
Attack-10 verdict (two threads binding concurrently without any lock; MVCC
breach by the writer) — **held**, premise impossible.

### INV-S9-5 — the fork protocol: the child never touches the parent's coordination state

`pthread_atfork(rthc_beforefork, rthc_afterfork_parent, rthc_afterfork)`
(rthc.c:531). In the child (`rthc_afterfork`, rthc.c:488-524): the process
mutexes that may be recorded under the parent's thread id are **reinitialized**
(`globals.debug_lock`, `rthc_mutex`, `rthc_cond`, per-env `dbi_lock` /
`remap_lock`), the inherited write transaction's ownership is re-pointed to
the surviving thread (`basal_txn->owner = env->txn ? self : 0`) so
`mdbx_env_resurrect_after_fork` can abort it, both mappings are unmapped and
`env->lck` is re-pointed to the in-env `lckless_stub`. `rthc_drown` then skips
slot cleanup and `lck_destroy` skips the ipc-drown because
`current_pid != env->pid` (rthc.c:363, lck-posix.c:474) — the parent's
process-shared mutexes and SysV semaphore set are never destroyed by the
child. Closing the inherited fds is safe: classic fcntl locks are per-process
(the child holds none), OFD locks survive while the parent's fd keeps the open
file description alive. The child-side `lck_txn_unlock` of an inherited write
txn is covered by the fork guard in `osal_ipclock_unlock`
(lck-posix.c:1049-1051: `current_pid != env->pid → MDBX_SUCCESS`). A child
writer waiting on the inherited locked `wrt_lock` is proper cross-process
exclusion via the shared futex; recovery after the parent's death requires the
robust flavor (POSIX2008, auto-selected; POSIX2001/SysV wait until the parent
unlocks or the flavor's dead-owner mechanism fires — SysV: SEM_UNDO rollback
+ stale pid word → EOWNERDEAD).

### INV-S9-6 — flavor-selection `#error` sites are compile-time guards, all N/A

All ten `#error "FIXME"` sites (layout-lck.h:35; lck-posix.c:217, 227, 810,
896, 960, 1022, 1045) fire only for an `MDBX_LOCKING` value outside the
covered set. The autodetection (options.h:397-431) selects only
WIN32FILES / POSIX2008 (robust, with the glibc ≥ 2.10 and non-OHOS gates) /
POSIX2001 / POSIX1988 (Solaris+Haiku, where SysV cannot be used because the
lock-file format must not change) / SYSV. `MDBX_LOCKING_FUTEX` is a declared
value never selected by autodetection; reaching its `#error` requires an
explicit `-DMDBX_LOCKING=…FUTEX` build misconfiguration and fails at compile
time. No runtime-reachable FIXME.

---

## 2. Coordination-primitive contracts (additions for `module-interfaces.md`)

### `mvcc_bind_slot(env) -> bsr_t`
*Pre:* `env->lck_mmap.lck != nullptr` (asserted), env not FATAL, dxb mapped;
caller holds no reader-table mutex. *Post (success):* the returned slot is
claimed by `env->pid` with poisoned txnid; `thread_rthc_set(me_txkey, slot)`
when `ENV_TXKEY`; the caller publishes the real txnid in `ro_seize`.
*Post (error):* no state change (all failures happen before the pid publish or
leave `pid == 0`); `MDBX_READERS_FULL` when the table is full of live readers,
any `lck_rpid_check`/lock error is propagated. Errors: `MDBX_PANIC`
(ENV_FATAL), `MDBX_EPERM` (unmapped dxb), lock errors, `READERS_FULL`.

### `mvcc_snapshot_oldest_rw(txn, may_lag) -> orsi_rw_t`
*Pre:* write txn, writer mutex held. *Post:* `oldest_txnid ≥ steady.txnid` is
**not** guaranteed on the face value — the contract is: `oldest_txnid` is a
*valid lower bound* of the true oldest, equal to it when a stable pass ran;
the stored cache is updated monotonically except the deliberate distrust reset
(PAIR-N10); `may_lag` bounds the scan to 4 passes and returns `prev_oldest`
(#325), which is itself a valid lower bound under the cache discipline and is
suppressed when the cache is distrusted. Never returns a value above
`steady.txnid` as trusted.

### `mvcc_cleanup_dead(env, rdt_locked, *dead) -> rc`
*Pre:* `rdt_locked` ∈ {0 = not held, 1 = held, -1 = acquired inside}; env
check passed. *Post:* every slot whose pid is verified dead is cleared
(`pid = 0`, refresh flag set); `*dead` = count; self slots skipped; on lock
recovery (`lck_rdt_lock == MDBX_RESULT_TRUE`) returns `MDBX_RESULT_TRUE` —
the recovery already checked all readers. Failure (`rc != SUCCESS`) means
"unknown state, do not proceed" for `mvcc_bind_slot` (READERS_FULL) and
"stop kicking" for `mvcc_kick_laggards`.

### `mvcc_kick_laggards(txn, straggler, obstacle) -> bool`
*Pre:* writer mutex held; `straggler` = detent/oldest blocking GC.
*Post:* returns `oldest > straggler`; every `true` return has advanced the
cache (PAIR-N10). HSR contract on the libmdbx side: the callback runs
**synchronously with the writer mutex held**; `(intptr_t)pid <= 0` forged pids
never reach it (mvcc-readers.c:461); parked readers are ousted by CAS
(`PARKED → OUSTED`) followed by `safe64_reset_compare` (INV-T8 family); the
loop is bounded by `INT_MAX` retries (V-036) plus the cache-advance bound.

### `lck_seize / lck_downgrade / lck_upgrade` (POSIX)
*Post seize TRUE:* lck byte0 `F_WRLCK` + dxb full lock — the caller is the
initializer. *Post seize FALSE:* lck byte0 `F_RDLCK` + dxb byte-`pid` lock.
*Post downgrade:* lck byte0 `F_RDLCK` + dxb byte-`pid` (the fcntl split of the
full-file unlock leaves exactly `[pid, pid+1)` locked). *Upgrade failure:* the
rollback restores the shared state (and `MDBX_BUSY` on contention,
`MDBX_PANIC` when the restore itself fails). The dxb-collision deadline (2 s)
bounds the seize retry loop against a without-lck dxb holder.

### `lck_destroy(env, neighbor, pid)`
*Post:* if `pid != env->pid` (forked child) — skip drown entirely (fds still
closed). If a neighbor exists — skip drown and, for classic (non-OFD) locks,
restore the neighbor's dxb/lck/rpid locks after closing our fds; any restore
failure poisons the neighbor with `ENV_FATAL_ERROR`. If exclusive was acquired
(no other process open) — destroy the shared ipclocks / IPC_RMID the SysV set,
munmap, ftruncate the lck to 0.

### `rthc_thread_dtor(rthc)` / `rthc_dtor(pid)` / `rthc_drown(env)`
*Post dtor (thread exit):* every env of this process with `ENV_TXKEY` loses
the thread's slot (`pid CAS→0`) + refresh flag; the thread's rthc state
signature is consumed (registered/counted), `rthc_pending` accounting
balanced. *Post rthc_dtor (process exit):* waits (≤ 100 ms + 3 sched_yield,
the glibc-21031 workaround) for fallback-key users, deletes `rthc_key`, and
clears every slot of the current pid for every env. *Post drown:* all slots of
`current_pid` cleared, pid-byte cleared, `lck_destroy` invoked, `env->pid = 0`.

---

## 3. Findings (V-049…V-052) and hardening notes

| V-id | Where | Суть | Severity | Machine check |
|---|---|---|---|---|
| V-049 | mdbx.h:1180-1183 vs src/osal/posix/osal-fs.c:15-28 | **Doc/code divergence:** the promise "pure read-only mode when environment directory and/or both files are not writable" is not honored — the POSIX code grants lckless only on `ST_RDONLY` (fs mount flag); a non-writable directory on a writable filesystem fails `env_open`. Docs broader than the code (code stricter — safe direction, but the promise is false) | low/doc | repro E3 |
| V-050 | mvcc-readers.c:282-376 vs :461 | **Hardening asymmetry:** `mvcc_kick_laggards` guards `(intptr_t)pid <= 0` (wave-1 verified), `mvcc_cleanup_dead` does not: a crafted slot pid ≥ 2³¹ reads negative (`mdbx_pid_t = pid_t`), reaches `lck_op` with a negative `l_start` (the `ASSERT(pid > 0)`/`ASSERT(offset >= 0)` guards are checking-builds only) → kernel `EINVAL` → cleanup errors out → every subsequent reader bind fails (`EINVAL`/`READERS_FULL`) and kick aborts. Bounded liveness DoS from a hostile lock-file writer; no memory unsafety (all loops rdt_count-clamped). Fix form: treat `pid <= 0` slots as stale and clear them | low | code-probe (by-model crafted lock file) |
| V-051 | mdbx.h:7459-7540 vs lck-posix.c:988-1027 | **HSR re-entrancy contract undocumented:** the callback runs with the writer mutex held, but mdbx.h never states the restriction. Observed failure modes of a callback that begins a write txn on the same env: POSIX2001/2008 → self-relock `EDEADLK`, exempt from `ENV_FATAL_ERROR` (lck-posix.c:964-965) → clean fail-safe error; POSIX1988/SysV (semaphores) → `sem_wait`/`semop` self-block → **hang**; Windows → recursive `CRITICAL_SECTION` + per-handle `LockFileEx` re-lock likely succeeds (lock ownership is per-handle) → likely re-entrant entry. Document: "must not call mdbx functions requiring the write transaction lock" | low/doc | code-reading |
| V-052 | lck-posix.c:285-418, 420-468 | **Trust boundary of the lck file (by-model):** replacing the lck file under a live env splits the coordination domain. A *fresh* (empty) replacement is defused: the old env's dxb byte-`pid` lock blocks every new-world exclusive seize, and check_fstat keeps returning "too short", so new-world openers exhaust the 2-second dxb-collision deadline → `MDBX_BUSY` (bounded, not livelocked). Restoring a **copy** of the old lck file bypasses the size check (valid magic/format/size); the shared-return path of `lck_seize` (the final "lock-against-without-lck" block) then completes *if at least two new-world openers race* — opener A holds byte0-RDLCK so opener B's exclusive attempt fails with a sharing violation and B returns `MDBX_RESULT_FALSE` without ever having taken the dxb exclusive — yielding two worlds with separate `wrt_lock` mutexes → concurrent write txns, silent corruption. Requires external file manipulation (mv/cp of the lck) plus a two-opener restart — same trust level as crafting the lock file, but no write access needed, only directory rename. Potential hardening: stamp the dxb fileid into the lck header and verify at seize | low | code-probe |

### Verified-held highlights (attacks 1-10, one line each)

1. **Crash mid-bind** — held: poison-before-publish (INV-S9-1); crashed
   publisher leaves `pid == 0` or `pid != 0 && poisoned-txnid`, both invisible
   to scans and reclaimable after the pid-byte dies with the process.
2. **Crafted rdt_length=32767 / self-pid slots** — held for memory safety
   (INV-S9-2, every loop clamped incl. cleanup_dead's inner `rdt[ii]` loop);
   the crafted-self-pid slot is skipped by `pid == env->pid` and pinned alive
   for others — bounded GC-retention DoS, same trust model as V-034.
3. **Non-atomic `rdt_length.weak = ++nreaders`** — held (INV-S9-2): single
   writer site under the reader mutex; 32-bit aligned store; all readers
   clamped; the refresh-flag protocol concerns the oldest cache, not the
   occupancy.
4. **mutex_failed recovery + pid reuse** — held (INV-S9-3): robust EOWNERDEAD
   recovery checks all readers (cleanup runs before `pthread_mutex_consistent`);
   pid reuse yields false-live (leak) only; live readers never false-dead.
5. **OFD downgrade/upgrade race** — held: downgrade is a self-conversion after
   the dxb split (byte-pid remains); upgrade contention → EAGAIN/EDEADLK →
   `MDBX_BUSY`; failure rollback restores the shared state.
6. **SysV ftok collision (#80 family)** — held: signature chunks + `sem_ctime`
   stability re-check (8 attempts, bounded) decide reclaim-vs-flee; the
   creation race cannot interleave with a concurrent initializer because
   `lck_init(TRUE)` runs only under the seized exclusive lck lock, and the
   downgrade (env.c:538) happens after the SETALL.
7. **fork in a reader thread** — held (INV-S9-5): child reinitializes
   process mutexes, never destroys parent's ipc locks (pid guards at
   rthc.c:363/474 and the osal_ipclock_unlock fork guard); resurrect re-opens
   under the child's pid; `env_close` in the child is legitimized via
   `MDBX_ENV_CHECKPID` + drowned state.
8. **TLS destructor race (#21031 family)** — held: `rthc_thread_dtor` takes
   `rthc_mutex`; `mvcc_cleanup_dead` never takes it; the slot-clear race with
   `mvcc_bind_slot` is resolved by the pid CAS (a dying thread's slot is
   either still owned or already zero — a concurrent bind claims only zeroed
   slots and re-poisons); the dtor's `pthread_setspecific` failure path covers
   the tsd-key deletion race with `mdbx_env_close` (env.c:563-565 deletes the
   key before `rthc_remove`, and `rthc_remove`→`rthc_drown` clears all of the
   process's slots anyway).
9. **HSR re-entrancy** — held for POSIX2001/2008 (fail-safe EDEADLK), hang on
   POSIX1988/SysV and re-entry on Windows — documented as V-051 (doc gap).
10. **lckless multi-thread binds** — held (INV-S9-4): binding does not exist
    in lckless mode; lckless implies a read-only filesystem → no writer can
    coexist. Machine-checked: `repro/v8b-lckless-reader-mvcc.c`.

---

## 4. Test-coverage gaps (platform-specific, for `test-coverage.md`)

| Gap | What to test | Where |
|---|---|---|
| G-S9-1 | **OFD-vs-classic fcntl fallback**: `lck_op`'s ENOSYS/EINVAL fallback to `F_SETLK` (lck-posix.c:133-147) changes the whole-process lock semantics (the close-releases-all-locks hazard lck.c:134-139 describes); no test exercises the fallback path | requires a kernel/fs where OFD fails, or a fault-injected `fcntl`; unit-testable via MPROBE |
| G-S9-2 | **EOWNERDEAD recovery**: no test kills the holder of `wrt_lock`/`rdt_lock` cross-process and asserts the robust recovery (`osal_ipclock_failed` → cleanup_dead → consistent) preserves the reader table and the writer proceeds | multi-process UT (fork + kill -9 of the writer, POSIX2008 flavor only) |
| G-S9-3 | **SysV flavor**: `sysv_ipc_eexist_decide` reclaim-vs-flee (signature match/mismatch, the 8-attempt ctime race, `IPC_RMID` EIDRM) is only covered by the MPROBE `ftok_key_collide` hook in probe builds; no multi-process test exists for the SysV flavor on Linux | flavor-parameterized UT (`MDBX_LOCKING=5`) |
| G-S9-4 | **fork matrix**: child close without resurrect, child resurrect with inherited active write txn (the `basal_txn->owner` reassignment), child RO txn abort via `txn_abort_after_resurrect` — covered only partially by `mdbx_env_resurrect_after_fork` smoke tests | tests/ut (fork + resurrect + close matrix) |
| G-S9-5 | **Thread-exit slot cleanup under churn**: N threads begin/abort RO txns while the writer churns GC; assert no slot leak (`mdbx_reader_check == 0`) and no `READERS_FULL` over 10⁴ cycles — the rthc_thread_dtor ↔ mvcc_bind_slot race has no dedicated test | stress UT |
| G-S9-6 | **Windows-only paths** (`flock_ex` overlapped-cancel completion, the `lck_event_cs`/`dxb_event_cs` LockFileEx+WriteFile workaround, `osal_suspend_threads_before_remap` resume-on-failure) have no Windows CI coverage in this campaign — verified by code reading only (PROVED·A(win-reading)) | Windows CI job |
| G-S9-7 | **pid-reuse retention**: a stale slot whose pid is reused by a new live reader must pin GC (leak, no corruption) — hard to test deterministically; a model/simulation test of `mvcc_cleanup_dead` + `lck_rpid_check` semantics would pin INV-S9-3 | simulation/skip |

---

## 5. Cross-checks with prior waves

* PAIR-N10/INV-C4 (cached-oldest discipline) re-confirmed at statement level in
  `mvcc_snapshot_oldest_rw` incl. the `#325` 4-pass bound and the
  distrust-suppression of the bound (mvcc-readers.c:133-144).
* V-034 re-confirmed: the RO fatality for a forged cached-oldest is the
  `ENSURE_OBJ(txn->txnid >= cached_oldest)` pair in `txn_ro_start`/`ro_slot_clean`
  (txn-ro.c:283-284, txn-ro.c:121) — unchanged by this wave.
* L2 (`rdt_count`) re-confirmed as the single clamp point; V-050 is the first
  found consumer-side gap of the *pid* domain (not the length domain).
* INV-T8 (unpark base-ABA) reused for the kick/oust-CAS analysis
  (mvcc-readers.c:418-446, txn-ro.c:361-375).
