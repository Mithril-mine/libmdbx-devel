# Doc additions — tranche V11c (stream C: OSAL primitive contracts, posix + windows)

Scope: contract-consistency + statement-level verification of `src/osal/` primitives
(`osal.h`, `details.c`, `osal/posix/*`, `osal/windows/*`) against their engine
call sites (`page-iov.c`, `dxb-resize.c`, `dxb.c`, `txn.c:txn_setup_primal`,
`txn-ro.c`, `meta.c`, `lck.c`, `coherency.c`, `lck-windows.c` suspend/resume).
Windows items are **code-reading only** (no Windows CI), marked PROVED·A(win-reading).
Machine-checked POSIX evidence: `repro/v11c-osal-primitives-posix.c` (exit 0;
E1 on tmpfs and ext4, E2, E3).

Verdicts: **CONSISTENT** = contract holds (proof or machine-check);
**CONSISTENT-A** = holds with assumptions labeled (windows code-reading /
documented OS semantics); **INCONSISTENT** = concrete violating scenario exists.

---

## 1. Per-primitive ledger

### osal_pwrite / osal_pwritev / osal_pread / osal_write (posix) — CONSISTENT
Contract (osal.h:452-459): full-transfer or error; `pread` returns MDBX_ENODATA
on short-read (page beyond EOF); pwrite loops over partial writes, retries EINTR,
chunks to MAX_IO_BYTES.
Implementation (posix/osal-file.c:226-299): pwrite loop advances
buf/offset/bytes; pread is a single `pread` — POSIX gives short reads only at
EOF, so ENODATA is exact. `pwritev` fallback path (no PWRITEV) loops
osal_pwrite per segment; PWRITEV path loops on EINTR only and maps a partial
`pwritev` (written ≥ 0 < expected) to MDBX_EIO — the already-written prefix
stays, which is safe by the crash-safety model (commit aborts before the meta
write; orphan pages are reclaimed by GC).
Engine call sites: `osal_ioring_write` (posix/osal-ioring.c:129-158) breaks on
first error; `iov_write` (page-iov.c:131-141) propagates r.err and the
mid-commit flush failure is now propagated by `iov_page` (page-iov.c:164-172,
previously dropped — fixed per the in-code comment). `dxb_read_header`
(dxb.c:55-100) consumes ENODATA for the empty-file case and double-reads each
meta page. Consistent with wave 1-4 findings (retry semantics verified).

### osal_pwrite / osal_pread / osal_pwrite_ev / osal_pread_ev (windows) — CONSISTENT-A
PROVED·A(win-reading). Contract as above; windows overlapped handles require
an OVERLAPPED per operation.
Implementation (windows/osal-file.c:153-238): OVERLAPPED zeroed per attempt
(Internal/InternalHigh/Offset/OffsetHigh/hEvent); ERROR_IO_PENDING →
`GetOverlappedResult(fd, &ov, &n, true)` — bWait=true waits on the *file
handle*, so hEvent may be NULL or even a shared event without cross-operation
reuse; failure cancels via `CancelIo` (thread-scoped — the same thread has no
other in-flight IO on this handle during commits, since txn_write/pwrite are
thread-serialized by the writer lock). Short WriteFile → loop continues
(partial-write contract mirrors posix). ENODATA on short read at EOF — same
engine consumption as posix. `dxb_pread/dxb_pwrite` (dxb.c:28-34) route
through the overlapped handle and pass `env->ioring.async_done` as hEvent;
harmless: the kernel may SetEvent it, and `osal_ioring_reset` ResetEvents it
before each batch; defrag (the only dxb_pwrite/dxb_pread user) is
writer-serialized against ior_write on the same thread.

### osal_dxb_write_stub / stubv (both) — CONSISTENT
Both delegate to osal_pwrite/pwritev through the handle that owns the per-txn
body lock (windows: overlapped_fd if present, else lazy_fd) — byte-range locks
are per-handle on Windows, so any other handle would fail ERROR_LOCK_VIOLATION
(windows/osal-file.c:240-261, posix/osal-file.c:301-318). Statement-level:
the comment's claim that pwrite on windows waits on the handle (not a shared
event) matches osal_pwrite_ev, so no shared-event reuse across segments.

### osal_fsync (both) — CONSISTENT (with a documented SIZE-only no-op on Linux)
Mode decode (osal.h:469-475): NONE=0 KICK=1 DATA=2 SIZE=4 IODQ=8.
Posix (posix/osal-file.c:320-357): NONE/KICK → no-op; DATA → fdatasync (loop on
EINTR); DATA|SIZE and everything else → fsync; **SIZE alone on Linux →
unconditional SUCCESS** (fdatasync has persisted size metadata since kernel
3.6; guarded by debug-only ASSERT). Engine never constructs SIZE-without-DATA
(dxb.c:977-983 always starts from SYNC_DATA; meta.c uses DATA|IODQ; api-copy
DATA|SIZE → fsync; env.c DATA; lck.c msync DATA|SIZE), so the branch is
dead-in-practice (finding F-C11c-7, info). Windows (windows/osal-file.c:263-267):
DATA or IODQ → FlushFileBuffers (persists data+size metadata); NONE/KICK →
no-op — mirrors posix SYNC_KICK semantics (verified wave 1-4).

### osal_msync (both) — CONSISTENT
Posix (posix/osal-mmap.c:15-36): `!MDBX_MMAP_NEEDS_JOLT && SYNC_NONE` → no-op
(the intentional SYNC_NONE no-op, wave 1-4); otherwise msync(MS_SYNC iff DATA
else MS_ASYNC) + fsync(fd) iff SIZE. Windows (windows/osal-mmap.c:70-79):
FlushViewOfFile(base, length) always; FlushFileBuffers iff DATA|IODQ.
Engine: lck.c:117 msync(lck, DATA|SIZE); dxb_msync KICK for the resize
pre-kick (dxb-resize.c:237) and DATA|IODQ for meta persistence (meta.c).

### osal_fsetsize (posix) — CONSISTENT
(posix/osal-file.c:389-469) fstat → allocated = 512×st_blocks; if length >
allocated: fallocate(ALLOCATE_RANGE|KEEP_SIZE) (Apple: F_PREALLOCATE with
F_ALLOCATEALL fallback — the chunked-allocation TODO remains, benign since the
fallback covers it); ENOSYS/EOPNOTSUPP-class errors ignored → ftruncate.
ENOSPC is ignored only when osal_check_fs_incore() == TRUE (tmpfs/ramfs —
test-environment workaround). Linux 5.10.x + ext4: fdatasync before ftruncate
(the #444-era size-persistence workaround window `>= 0x050a0000 &&
< 0x05110000`). VERIFY: bounds correct; MPROBE fsetsize_fallocate_errno
preserves the errno contract.

### osal_fsetsize (windows) — CONSISTENT-A
PROVED·A(win-reading). SetFileInformationByHandle(FileEndOfFileInfo) or
SetFilePointerEx+SetEndOfFile fallback (XP). Sets EOF only (no preallocation —
windows needs none for correctness; write-time ENOSPC surfaces as write errors,
handled by the crash-safety model).

### osal_filesize / osal_fseek / osal_fseek_shut (both) — CONSISTENT (E1 machine-checked)
filesize: fstat st_size / BY_HANDLE_FILE_INFORMATION. osal_fseek_shut
(details.c:15-47) parks the fd position outside the data to guard against
stray writes via dup'd/app-damaged fds; on EINVAL (target beyond the FS limit)
binary-searches shifts for the largest seekable probe, re-seeks explicitly to
`best`. E1: ext4 converged to 0x80000000000 (8 TiB cap), stable re-park,
payload intact; tmpfs accepted 2^62 directly (fallback unreachable there).

### osal_mmap (posix) — CONSISTENT
(posix/osal-mmap.c:38-146) Resets map fields first (union base/lck nulling —
R-C3 corroboration, wave 4c). RDONLY/non-SETLENGTH: current = min(filesize,
limit) — a POSIX MAP_SHARED mapping reflects file growth up to `limit`
automatically; this is what makes reader grow-follow sound on POSIX.
SETLENGTH: fsetsize(size), current = size. ASAN/Valgrind retry halves limit.
MADV_DONTFORK failure returns an error with the map fields still populated
(F-C11c-5, info: caller-side munmap on failed open is required to avoid a VA
leak; osal_munmap(nullptr-safe) makes a double-clean safe).

### osal_mmap (windows) — CONSISTENT-A with findings (F-C11c-1)
PROVED·A(win-reading). NtCreateSection(SEC_RESERVE, MaximumSize = size) +
NtMapViewOfSection: WRITEMAP maps a MEM_RESERVE view of `limit` (pages
materialize as NtExtendSection grows the section — the documented incremental
growth trick; Wine falls back to view = size); RDONLY maps with ViewSize = 0,
i.e. **the view is fixed at the section's size = the file size at open**
(Windows file-backed section views do not track later file growth; MSDN
MapViewOfFile semantics). Consequently on native Windows RDONLY
`map->limit == map->current == open-time size`, while POSIX RDONLY has
`limit == requested limit`. See F-C11c-1 for the grow-follow consequence.

### osal_munmap (both) — CONSISTENT
Nulls limit/current/base after unmap (posix munmap; windows NtClose(section) +
NtUnmapViewOfSection). base/.lck are a union, so both are nulled — R-C3
(double-munmap hypothesis) stays refuted, now corroborated for the windows
side as well (windows/osal-mmap.c:172-190).

### osal_mresize (posix) — CONSISTENT
(posix/osal-mmap.c:163-383) Fresh filesize first; RDONLY: never grows the view
beyond the current mapping (`mapped = min(limit, map->limit)`), EPERM when the
file shrank externally beyond the mapping; writer: fsetsize extend (shrink only
iff MAY_SHRINK), current = min(size, limit). Limit grow ladder:
mremap(MAYMOVE iff MAY_MOVE) → on EFAULT append-mmap at base+limit with
MAP_FIXED_NOREPLACE (EEXIST/EINVAL tolerated) → unmap+remap-whole iff
MAY_UNMAP else MDBX_UNABLE_EXTEND_MAPSIZE → on total failure restore the old
mapping and report UNABLE_EXTEND_MAPSIZE, zero the map only if even the
restore failed. MPROBE mresize_mremap_errno/mresize_mmap_errno cover the
restore path (issue gh0334 regression test exists). Engine decode
(dxb-resize.c): MAY_UNMAP/MAY_MOVE cleared when `env->txn` is active
(cursor stacks point into the map); readers-table lock + same-process reader
scan veto otherwise. Consistent with the wave 1-4 flags contract.

### osal_mresize (windows) — CONSISTENT-A with findings (F-C11c-1, F-C11c-4)
PROVED·A(win-reading). Fast path (limit unchanged, size > current):
RDONLY + filesize ≥ size → `current = size` **without any section/view
extension** — unreachable through the engine (windows RDONLY limit == current,
shown above) and unsound if ever reached (F-C11c-1b); writer + NtExtendSection
(not under Wine) extends section+file in place. Otherwise: address-space
pre-probe (NtAllocateVirtualMemory MEM_RESERVE probe at base+limit, the
INV-T8-adjacent reserve-old-base trick), MAY_UNMAP == 0 → SUCCESS for a same-
limit shrink-noop request, MDBX_EPERM for everything else (the documented
"cannot without un-mapping" code, per mdbx.h:3832-3838 and dxb-resize.c:25-36);
with MAY_UNMAP: unmap → NtClose(section) → reserve-old-base (holds the address
range across the slow fsetsize; on CONFLICTING_ADDRESSES with MAY_MOVE restart
at a new base) → fsetsize (errors ignored: the kernel cannot shrink a mapped
file) → new section → free reserve → map; on failure remaps at the old
size/limit and returns UNABLE_EXTEND_MAPSIZE/EPERM, zeroing the map only when
unrecoverable. Thread suspension is the engine's job and is done in
dxb_resize before requesting MAY_UNMAP (see osal_suspend_threads below).
F-C11c-4: MAY_SHRINK is not consulted in the unmap path (fsetsize targets
`size` unconditionally) and a same-limit shrink request silently succeeds
without shrinking — divergence from POSIX, engine-visible only through
filesize bookkeeping (safe direction, no corruption).

### osal_suspend_threads_before_remap / osal_resume_threads_after_remap (windows) — CONSISTENT-A
PROVED·A(win-reading), statement-level completion of wave-4 B.
(lck-windows.c:270-360) LCK path suspends exactly the registered same-process
reader threads (skips pid != env->pid, tid == 0 — the V-050 stale-pid
convention, tid ≥ MDBX_TID_TXN_OUSTED sentinel) plus the write-txn owner if
different; no-lck path (EXCLUSIVE|RDONLY) suspends all process threads via
toolhelp snapshot. MPROBE suspend_open_thread_error evaluated before any
suspension (a suspended thread could hold the probe-bus lock). Bailout resumes
everything (no orphan suspensions). SuspendThread failures tolerated for the
Win10 UCRT ACCESS_DENIED bug and for already-exited threads. Resume closes
handles and zeroes count, so a retry after a failed suspension is idempotent.
Residual (inherent to SuspendThread): a suspended thread holding a lock the
resizer later needs can deadlock — bounded by the try-acquire deferral in
dxb_resize (windows_remap_lock_exclusive, the #154 fix) and the 5-attempt
yield loop; lckless toolhelp suspension of arbitrary app threads is gated on
EXCLUSIVE|RDONLY.

### osal_ioring_add / walk / write / reset (posix) — CONSISTENT (E2 machine-checked)
(posix/osal-ioring.c) Slot accounting (INV-OS1): slots == sgv elements;
contiguous same-buffer merge extends the last iovec **in place, 0 slots**
(non-PWRITEV: extends single.iov_len); non-adjacent item costs 1; a merged
extension with no free slot returns MDBX_RESULT_TRUE before mutating anything;
the pool cannot overrun (each slot accounts sizeof(ior_item_t) ≥ per-element
bytes). MDBX_RESULT_TRUE on full — engine flushes (page-iov.c:158-178) and
re-adds. write(): sequential osal_pwrite/pwritev, wops counted, break on first
error (caller drain contract = wave 2, via iov_write/iov_complete). reset():
slots_left = allocated, last = nullptr. `boundary = pool + allocated` (bytes,
not items×sizeof) is dead on both platforms — never read (F-C11c-8, info).

### osal_ioring_add / walk / write / reset (windows) — CONSISTENT-A (statement-level; #449 fix site)
PROVED·A(win-reading). Encoding (INV-OS2): single-buffer item ⟺
`single.iov_len = bytes + 1` (bytes is page-multiple, low bit set); gather
item ⟺ `iov_len == 0`; decoders compute `iov_len − 1` and test the low bit
(0−1 = SIZE_MAX has the bit set; payload−1 is even) — verified consistent
across add (windows/osal-ioring.c:122-152), walk (:155-187), write
(:233-328), drain (:195-224) and reset (:428-468). Gather requires direct +
overlapped_fd + page-aligned data/bytes/last_bytes/Buffer and
`last_sgvcnt + segments < OSAL_IOV_MAX` (= 512); variable-length item
accounting is safe (each slot accounts ≥ 8 bytes per segment).
write(): every item gets `ov.Internal = STATUS_PENDING` before submission;
gather on immediate success recycles its event; WriteFileEx uses hEvent = ior
(sentinel, never recycled) + ior_wocr APC (alertable waits only: the final
WaitForMultipleObjectsEx and drain's SleepEx are alertable); retryable errors
(ERROR_NOT_FOUND/USER_MAPPED_FILE/LOCK_VIOLATION) re-submit after
SleepEx(0,true). Batch wait (INV-OS3, the #449 fix): async_done is pushed
**last** into wait_for so it lands within the first MAXIMUM_WAIT_OBJECTS —
a mixed batch can no longer return while WriteFileEx APCs are in flight
(comment windows/osal-ioring.c:333-338); after the wait, gather items are
force-completed via blocking GetOverlappedResult and every item's Internal/
InternalHigh is verified (WRITE_FAULT on byte-count mismatch). All failure
exits route through CancelIo + ior_drain before returning, and
osal_ioring_reset cancels + drains + recycles events — the cancelled-request
reuse hazard of #449 is closed statement-level. Residual risks (bounded,
documented): ior_drain gives up after 10000 spins (~10 s) with a WARNING while
a driver-stuck request may still be outstanding (F-C11c-2); a wocr APC
delivered after drain's `!pending` exit but before reset zeroes
async_completed makes the next batch's done-event fire one completion early —
release builds then take the STATUS_PENDING error path (failure-safe, per-item
checks, no corruption) (F-C11c-3, hardening: one extra alertable SleepEx(0)
after `!pending` closes the window).

### osal_monotime / osal_16dot16_to_monotime / osal_monotime_to_16dot16 (both) — CONSISTENT
Posix: CLOCK_MONOTONIC nanoseconds (CLOCK_REALTIME fallback only where
CLOCK_MONOTONIC is undefined); windows: QPC ticks. 16.16 conversions are
exact with a ±1 floor; `monotime_limit` clamps the down-conversion to
UINT32_MAX; check_monotime_scale() ENSUREs the round-trip at ctor on both
platforms. Consumers: coherency_timeout (coherency.c:91-106) implements the
0.1 s coherency workaround as `osal_16dot16_to_monotime(65536/10)`; the
timestamp==0 first-call convention and unsigned-underflow bail-out make a
hypothetical clock failure (osal_monotime → 0) fail-safe (F-C11c-6, info).

### osal_getpid / osal_thread_self (both) — CONSISTENT
mdbx_pid_t ≤ 32-bit (posix: pid_t signed int32; windows: DWORD); getpid never
≤ 0 on either platform — corroborates the V-050 fix shape (pid ≤ 0 read as
stale) and INV-S9-3's pid-gate. osal_thread_self: pthread_t / GetCurrentThreadId
widened to uintptr_t; MDBX_TID sentinels (OUSTED = MAX−1, PARKED = MAX,
layout-lck.h:169-172) can never collide with a real posix pthread_t value or a
32-bit Win32 TID; windows suspend/resume scans filter `tid ≥ OUSTED`
(lck-windows.c:284). TID recycling is gated by the pid byte-lock
(INV-S9-3) — false-live only, safe direction.

### osal_bootid / osal_guid (both) — CONSISTENT (E3 machine-checked for posix)
Posix: /proc/sys/kernel/random/boot_id via proc_read_uuid (O_NOFOLLOW +
procfs-magic check; tmpfs accepted only inside lxcfs containers). Crafted
content resistance (details.c:64-102): text UUID needs > 126 collected bits
(≥ 32 hex digits); shorter/garbage falls to the binary path which hashes the
bytes and gates the result on check_uuid()'s hamming-weight well-formedness;
all-zero rejected; n ≤ 15 rejected. Windows (win-reading): MachineGuid /
DigitalProductId / Prefetcher BootId+BaseTime registry values /
SystemTimeOfDayInformation BootTime−BootTimeBias / the st-up convergence
probe, with a FILETIME-epoch RTC sanity gate mirroring the posix
MDBX_TRUST_RTC checks. Zeroed bootid = "unknown" on both (engine disables
boot-id-based features).

### osal_check_fs_rdonly / incore / local (posix) — CONSISTENT
Posix (posix/osal-fs.c): rdonly = statvfs(pathname) then fstatvfs(handle),
ST_RDONLY gate — the V-049/INV-S9-4 contract (only a read-only *mount* yields
lckless; MDBX_EACCESS/EPERM mapping at :26). incore: fstatfs magic switch +
BSD fstypename. local: /proc/mounts fsname by st_dev + magic switch; NFS/
CIFS/NCP/SMB/fuse(non-fuseblk)/9P/ftp/http/sshfs → EREMOTE with the
MDBX_EXCLUSIVE escape hatch; ST_EXPORTED → MDBX_RESULT_TRUE gated by the
MDBX_ENABLE_NON_READONLY_EXPORT option at the osal_mmap consumer.

### osal_check_fs_rdonly / incore / local (windows) — CONSISTENT-A
PROVED·A(win-reading). rdonly = GetVolumeInformationByHandleW
FILE_READ_ONLY_VOLUME gate (MDBX_ENOSYS below Vista — lckless unavailable
there, documented gap F-C11c-10); local: wine → ERROR_NOT_CAPABLE unless
EXCLUSIVE; remote-protocol/WOF/compressed-volume/DRIVE_REMOTE gates with the
EXCLUSIVE+RDONLY / EXCLUSIVE+UTTERLY_NOSYNC F_ISSET (ALL-bits) escape hatches;
incore is a constant MDBX_RESULT_FALSE (windows has no tmpfs-equivalent
check — the fsetsize ENOSPC workaround is posix-only, consistent).

### osal_openfile / closefile / lockfile (both) — CONSISTENT
Posix #144 safeguard verified statement-level (posix/osal-file.c:94-196): /dev/null
stubs for closed std fds, dup() escape, EBADF rejection if the race wins,
unlink of exclusively-created files on every failure path. Exclusive creation
cleanup mirrors on windows (CreateFileW CREATE_NEW + DeleteFileW on the
GetFileInformationByHandle failure path). Exclusive fastpath (windows):
shareable data-file opens + constant byte-range locks (windows_seize_exclusive,
lck-windows.c:206-241, meta via fd4meta / body via fd4data, per-handle lock
semantics respected, MDBX_BUSY on sharing violation, rollback on partial
failure).

### condpair / fastmutex / threads (both) — CONSISTENT
Posix: pthread wrappers, errno-returning, error-faithful init ladders.
Windows: mutex + two auto-reset events emulate the condpair
(SignalObjectAndWait/WaitForSingleObject; a lost signal degrades to a spurious
wakeup, which cond semantics tolerate); fastmutex = CRITICAL_SECTION with
SEH (native or the hand-rolled TIB-chain resume-record) translating
STATUS_POSSIBLE_DEADLOCK → MDBX_EDEADLK; osal_safe_peek_int32 via
IsBadReadPtr+SEH (posix: write-to-/dev/null probe). stub SRWLock
(Brad Wilson scheme) has correct dont_wait rollback semantics; imports fall
back to stubs below Vista.

---

## 2. Findings

| F-id | file:line | severity | description |
|---|---|---|---|
| F-C11c-1 | src/osal/windows/osal-mmap.c:147,207-210,252-261; src/txn.c:297-306; src/dxb-resize.c:91 | major-if-confirmed (win-reading) | Native-Windows RDONLY reader cannot grow-follow the file: the RDONLY view is fixed at the open-time file size (NtCreateSection MaximumSize = geo.now; ViewSize = 0 maps the section as it existed), while POSIX maps the full requested limit and reflects growth. When another process grows the file beyond that extent, the reader's txn begin (txn_setup_primal → dxb_resize(flags=0), remap not requested) reaches windows osal_mresize without MAY_UNMAP → MDBX_EPERM (osal-mmap.c:261) → mdbx_txn_begin fails. Needs Windows runtime confirmation. Sub-finding (1b): the RDONLY fast path at :207-210 (`map->current = size` without section extension) is unreachable via the engine (windows RDONLY limit == current) and would be unsound if ever reached — it claims pages beyond the fixed view. |
| F-C11c-2 | src/osal/windows/osal-ioring.c:217-219,428-468 | low (win-reading) | ior_drain gives up after 10000 spins (~10 s) with a WARNING; reset then recycles events/OVERLAPPEDs while a driver-stuck request may still be outstanding — a late completion can signal a recycled event or scribble a reused OVERLAPPED. Bounded and logged; requires ≥ 10 s of un-cancelable IO (driver hang). |
| F-C11c-3 | src/osal/windows/osal-ioring.c:195-224,463-465 | low/hardening (win-reading) | Stale-APC window: drain exits on `!pending` without a final alertable flush; reset zeroes async_completed; a wocr APC delivered afterwards leaves the next batch's async_completed off by one, so async_done can fire one completion early and a still-pending WriteFileEx item is misread as STATUS_PENDING error in release builds (per-item checks contain it to a failure-safe commit error). Fix shape: `SleepEx(0, true)` after the `!pending` exit (or don't zero async_completed until drained). |
| F-C11c-4 | src/osal/windows/osal-mmap.c:310-316,252-254 | low (win-reading) | MAY_SHRINK not honored on windows: the unmap/remap path fsetsizes the file to `size` unconditionally (a shrink may execute without the flag), while a same-limit shrink request returns MDBX_SUCCESS without shrinking (POSIX shrinks or fails). Engine-visible only through filesize bookkeeping; no corruption (the kernel rejects shrinking mapped files anyway). |
| F-C11c-5 | src/osal/posix/osal-mmap.c:132-137 | info/hardening | MADV_DONTFORK failure returns an error with map->base/limit/current still populated (mapping leaked unless the caller munmaps), unlike the mmap-failure path which zeroes the map. Rare (ENOMEM-class only). |
| F-C11c-6 | src/osal/posix/osal-misc.c:174-188; src/osal/windows/osal-misc.c:161-166 | info | osal_monotime returns 0 on clock-API failure. Consumers are failure-safe (coherency_timeout treats 0 as a fresh timestamp; unsigned underflow bails out), unreachable in practice; contract should state "monotime is failure-safe, 0 only on a broken clock". |
| F-C11c-7 | src/osal/posix/osal-file.c:342-346 | info | MDBX_SYNC_SIZE-only on Linux is an unconditional no-op (fdatasync-since-3.6 assumption), release-guarded only by ASSERT. Engine never constructs SIZE-without-DATA (dxb.c:977-983; meta.c DATA\|IODQ; api-copy DATA\|SIZE), so dead-in-practice; document "SIZE implies a prior DATA on Linux ≥ 3.6". |
| F-C11c-8 | src/osal/posix/osal-ioring.c:34,187; src/osal/windows/osal-ioring.c:56,511 | info | ior->boundary = pool + allocated **bytes** (not items×sizeof) on both platforms; never read — dead field with a misleading value. |
| F-C11c-9 | src/osal/windows/osal-ioring.c:470-478,490-493 | info | Windows resize-shrink path (dead via prepare's grow-only rule): ior_cleanup closes pooled events [items, event_stack) and zeroes event_stack, so pooled events [0, items) leak until destroy (destroy's cleanup sees event_stack == 0). Unreachable today. |
| F-C11c-10 | src/osal/windows/osal-fs.c:18-19; src/osal/windows/osal-misc.c:204-215 | info | check_fs_rdonly returns MDBX_ENOSYS below Vista (no GetVolumeInformationByHandleW) — lckless unavailable there; osal_getenv_singlethreaded returns "" for unexpected errors (ERROR_OUT_OF_PAPER branch) where posix getenv returns NULL — init-stage only. |

---

## 3. New invariants

**INV-OS1 (ior slot accounting, both platforms).** Every osal_ioring_add
success leaves `used_slots ≤ allocated`, where used_slots counts sgv elements:
contiguous merge = 0 slots (posix extends the last iovec / windows appends
page-segments only under the gather preconditions), non-adjacent item = 1 slot
(posix) or `segments` slots (windows gather); MDBX_RESULT_TRUE is returned
before any mutation when no slot is available. Hence the variable-length item
pool cannot overrun its allocation. Machine-checked (repro E2).

**INV-OS2 (windows ior item encoding).** `single.iov_len = bytes + 1`
(bytes page-multiple ⇒ odd) marks a single-buffer WriteFile item; `iov_len == 0`
marks a WriteFileGather item; every decoder (`walk`, `write`, `drain`,
`reset`) uses `(iov_len − 1) & 1` for the discriminant and recomputes payload
bytes from the page count, so encoding and decoding agree at all five sites.
PROVED·A(win-reading).

**INV-OS3 (windows batch flush contract, the #449 shape).** Every
osal_ioring_write exit leaves all OVERLAPPEDs completed: the success path
waits (async_done pushed last ⇒ WriteFileEx APCs always inside the first
MAXIMUM_WAIT_OBJECTS handles; alertable wait loop) and force-completes gather
items via blocking GetOverlappedResult; every failure path (bailout_rc /
bailout_geterr) executes CancelIo + ior_drain before returning;
osal_ioring_reset cancels + drains before recycling events. No cancelled-
request reuse into a next batch, except the bounded residuals F-C11c-2/3.
PROVED·A(win-reading).

**INV-OS4 (fd parking lot).** osal_fseek_shut leaves the fd positioned
strictly outside the data region, is stable across invocations, and on
EINVAL converges to the largest seekable offset of the filesystem.
Machine-checked (repro E1 on tmpfs and ext4).

**INV-OS5 (mresize flag decode, cross-platform).** Posix: mremap(MAYMOVE) →
append at base+limit (MAP_FIXED_NOREPLACE) → unmap/remap-whole, each gated by
MAY_MOVE / MAY_UNMAP, with a full old-mapping restore before reporting
UNABLE_EXTEND_MAPSIZE; windows: in-place NtExtendSection only for a plain grow
within the current limit (never under Wine), everything else requires
MAY_UNMAP and reports EPERM ("deferred") / UNABLE_EXTEND_MAPSIZE per the
dxb-resize caller decode; the engine sets MAY_UNMAP only when local threads
are suspended (windows) or no in-process reader/write-txn can observe the
base (posix veto at dxb-resize.c:192-227). PROVED·A for the windows half.

**INV-OS6 (map extent asymmetry, windows RDONLY).** On native Windows an
RDONLY mapping's `limit == current == file size at open` (fixed section view),
while POSIX RDONLY maps the requested `limit` and reflects file growth within
it. All engine reader-grow code paths are written against the POSIX shape;
on Windows this reduces to F-C11c-1.

---

## 4. Contract text for module-interfaces.md (osal-primitive pre/post)

- `osal_pwrite(fd, buf, bytes, offset)`: pre — bytes ≤ MAX_IO_BYTES per chunk
  (caller may pass more; chunked internally); post — full transfer or errno /
  winerror; retried on EINTR (posix); safe on overlapped handles (windows).
- `osal_pread(fd, buf, bytes, offset)`: pre — bytes ≤ MAX_IO_BYTES; post —
  MDBX_SUCCESS iff exactly bytes read, MDBX_ENODATA on EOF-short, else error;
  no partial success.
- `osal_ioring_add(ior, offset, data, bytes)`: pre — bytes is a
  MDBX_MIN_PAGESIZE multiple and ≤ MAX_IO_BYTES, offset likewise aligned;
  post — MDBX_SUCCESS (queued; contiguous data merged), MDBX_RESULT_TRUE iff
  the ring is full (nothing queued); the buffer must stay alive until
  write/complete.
- `osal_ioring_write(ior, fd)`: post — every queued byte transferred (SUCCESS)
  or err ≠ SUCCESS with the failing prefix persisted and (windows) the whole
  batch drained; `wops` = operations issued; the ring must be reset()ed before
  reuse (windows: cancel+drain is part of reset).
- `osal_ioring_reset(ior)`: post — slots_left == allocated, last == nullptr,
  no outstanding completions reference the ring's events/OVERLAPPEDs
  (windows: CancelIoEx + drain first).
- `osal_mmap(flags, map, size, limit, options)`: pre — size ≤ limit;
  post — SUCCESS ⇒ base != nullptr, current ≤ limit, RDONLY current = data
  extent valid for reads; error ⇒ posix zeroes the map (except the
  MADV_DONTFORK path, F-C11c-5), windows zeroes the map on all
  unrecoverable paths.
- `osal_mresize(flags, map, size, limit)`: pre — size ≤ limit; post —
  SUCCESS ⇒ current ≥ min(size, platform extent), filesize ≥ size (writer);
  UNABLE_EXTEND_MAPSIZE/EPERM ⇒ mapping intact and usable (both platforms
  guarantee the old mapping on refusal — posix restores, windows never
  unmaps without MAY_UNMAP); error with base == nullptr ⇒ env is
  ENV_FATAL (dxb-resize.c:341-346).
- `osal_fsync(fd, mode)`: post — DATA (and IODQ) ⇒ data durable; SIZE ⇒
  size metadata durable (posix: relies on a prior DATA or the fsync branch;
  linux-only no-op; windows: FlushFileBuffers covers size).
- `osal_msync(map, length, mode)`: post — DATA ⇒ mapped range durable-writable
  content flushed (posix MS_SYNC / windows FlushViewOfFile [+FlushFileBuffers
  for DATA|IODQ]); KICK ⇒ advisory.
- `osal_fsetsize(fd, length)`: post — file size == length (best-effort
  preallocation; windows sets EOF only; posix tolerates ENOSPC on incore fs).
- `osal_fseek_shut(fd, &offset)`: post — fd position parked outside the data
  (largest seekable ≤ requested); stable.
- `osal_suspend_threads_before_remap(env, &array)` (windows): post — every
  same-process thread that could hold a map pointer is suspended (lck path:
  registered readers + write owner; lckless: all but self) or tolerated-dead;
  failure ⇒ everything resumed.

## 5. libmdbx-invariants.md additions

INV-OS1..INV-OS6 as in §3 (IOR slot accounting; windows ior item encoding;
windows batch flush/drain contract; fd parking lot; mresize flag decode;
windows RDONLY map-extent asymmetry). Review-profile rule (extends the
wave-2 drain contract): any new osal_ioring_write failure exit MUST reach
CancelIo + ior_drain (or reset) before returning; any new windows map-grow
path MUST either extend in place (NtExtendSection, shared remap-lock) or hold
the exclusive remap-lock with threads suspended.

## 6. Test-coverage gaps (platform tests)

1. **Windows reader grow-follow** (F-C11c-1): two-process test — writer grows
   the DB past a reader's open-time size, reader begins a txn and reads the
   new pages. Currently no Windows CI multi-process grow-follow test; the
   POSIX equivalent exists in the stress suite only.
2. **Windows mixed ior batch under fault injection**: WriteFileEx + gather mix
   with a failing tail item (needs a windows fault-injection probe site);
   verifies INV-OS3's drain on every error shape, incl. the premature
   async_done race (F-C11c-3) under a stress loop.
3. **Windows shrink deferral** (F-C11c-4): reader pins the remap-lock while a
   shrink commit runs — assert the documented EPERM-vs-success decode and the
   later re-attempt (mdbx.h:3820-3840 promises; only the gh0154/gh0170
   deadlock sides have tests).
4. **posix fsetsize on a capped tmpfs** (ENOSPC-incore workaround): /dev/shm
   sized below the requested geometry — assert the ftruncate fallback and the
   non-incore ENOSPC propagation. Machine-checkable, no CI cell yet.
5. **osal_fseek_shut on a capped filesystem** (E1 ext4 branch) as a unit test
   rather than a probe — the parking-lot search currently has no suite
   coverage.
6. **RDONLY map growth within limit on POSIX** (mresize RDONLY branch, incl.
   the EPERM external-shrink case) — statement-verified here, no test.

## 7. KB keys written

(Store truncates long keys; actual keys as stored below.)

- `fact:verification:osal` — master ledger summary (verdicts, INV-OS1..6, coverage gaps).
- `bug:osal:win-readonly-grow` — F-C11c-1 (+1b).
- `bug:osal:win-ior-drain` — F-C11c-2/3.
- `fact:verification:osal-mresize-shrink` — F-C11c-4 (+ minor F-C11c-5..10 notes).
