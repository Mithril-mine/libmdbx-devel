# Known defects found while building the golden suite (TASK-45)

These are REAL deviations/bugs of the CURRENT C-tools, surfaced by the
characterization tests. Per Phase B constraints the tools were NOT modified;
classification (bug vs spec error) is Phase C (TASK-46) work. Escalated here for
the rewrite (Phase D) and the owner.

## D1 — mdbx_chk -i segfaults on any DB containing named tables
- Repro: `mdbx_chk -i <db>` where db has ≥1 named sub-table.
- Observed: SIGSEGV, assert `MDBX-ASSERTION: tbl->cookie` at `chk_handle_kv()`
  `src/tools/chk.c:970`.
- Scope: reproduces on both mdbx_load-created and mdbx_test-created DBs; `chk`
  without `-i` passes the same DBs.
- FIXED (issue #49, C.ci-guru, devel): (a) `chk_db()` no longer passes custom
  comparators to `dbi_open()` under `MDBX_CHK_IGNORE_ORDER` — the engine cannot
  bind different comparators to an already-bound table via `MDBX_DB_ACCEDE`
  (`MDBX_INCOMPATIBLE`); order-tolerance is provided by the `z_ignord` cursor
  flag and the IGNORE_ORDER report guards. (b) `chk_handle_kv()` now skips
  per-record user processing for tables filtered out by `table_filter()`
  (NULL cookie) instead of asserting; this also fixed the `-s` crash (D2).
- The `chk/ignore_order` golden case is re-enabled.

## D2 — mdbx_chk -s <table> crashes on tables created via mdbx_load
- Repro: `mdbx_chk -s meta <db>` where `meta` was loaded with mdbx_load.
- Observed: SIGSEGV (same `tbl->cookie` assert family).
- On a table created by `mdbx_test --table=+data.fixed` the same command works
  (rc=0) — so the crash is specific to load-created table descriptors.
- FIXED together with D1 (issue #49): `chk_handle_kv()` tolerates filtered-out
  tables (NULL cookie); the `chk/specific_table` golden case still covers the
  mdbx_test-created `named.db` fixture and `-s meta` on `base.dump` is verified
  green in the fix validation matrix.

## D3 — mdbx_load fails to load from stdin
- Repro: `mdbx_load -nf <new-or-existing-db> < fixtures/base.dump`
  (no `-f file`), target may be fresh or existing.
- Observed: exit 1, `mdbx_load: <path>: open: No such file or directory`;
  strace shows an early `openat(<path>, O_RDONLY)` before any stdin read.
- Loading the SAME stream via `-f fixtures/base.dump` succeeds (rc=0) — the
  `-f`/`freopen` path is fine, the bare-stdin path is broken.
- **FIXED (issue #50, branch `fix/issue50-load-stdin`)**: root cause was the
  arg-parsing order — `-f` consumed the dbpath as its file argument, then
  `freopen()` opened the (nonexistent) target `O_RDONLY`. `src/tools/load.c`
  now defers the input-file `freopen` until after the dbpath is identified and
  recovers when `-f` consumed the only remaining argument (stdin-by-default
  per `mdbx_load.1`). The `load/stdin` golden case now records rc=0 and also
  asserts the loaded DB is identical to one loaded via `-f file`.
- Root cause analysis: the golden `case_load_stdin` intentionally invoked
  `-nf <db>` (compact combined flags); POSIX getopt binds the trailing
  argument to `-f`, leaving no dbpath. With the fix the last positional wins
  as the dbpath and stdin is used, per the documented default.

## Notes
- Severity: D1/D2 were crashers, both FIXED (issue #49) on devel with backports
  to master/stable/lts pending; D3 was a spec deviation (stdin-by-default) and
  is FIXED by issue #50 (devel@a14a8816).
- Suggested owner escalation: all three golden-suite defects are now resolved;
  no remaining Phase D escalation.

## Status tracking (2026-09-24)
- **D3 → FIXED** (issue #50, fix/issue50-load-stdin@6d3092cb merged devel@a14a8816;
  golden `case_load_stdin` rc 1→0; G.tester VALIDATED).
- **D2 → FIXED-by-#49** (issue #51 closed as duplicate of #49 by owner review;
  same `tbl->cookie` assert family; `specific_table` workaround remains).
- **D1 → FIXED** (issue #49, fix/issue49-chk-cookie; C.ci-guru: golden
  `case_chk_ignore_order` re-enabled and green, `case_chk_specific_table` green,
  bug confirmed present in stable/master/lts for backport).
