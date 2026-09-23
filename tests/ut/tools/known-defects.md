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
- The `chk/ignore_order` golden case is SKIPPED (documented in
  `golden-runner.sh`).

## D2 — mdbx_chk -s <table> crashes on tables created via mdbx_load
- Repro: `mdbx_chk -s meta <db>` where `meta` was loaded with mdbx_load.
- Observed: SIGSEGV (same `tbl->cookie` assert family).
- On a table created by `mdbx_test --table=+data.fixed` the same command works
  (rc=0) — so the crash is specific to load-created table descriptors.
- Worked around in the suite: the `chk/specific_table` case uses the
  mdbx_test-created `named.db` fixture.

## D3 — mdbx_load fails to load from stdin
- Repro: `mdbx_load -nf <new-or-existing-db> < fixtures/base.dump`
  (no `-f file`), target may be fresh or existing.
- Observed: exit 1, `mdbx_load: <path>: open: No such file or directory`;
  strace shows an early `openat(<path>, O_RDONLY)` before any stdin read.
- Loading the SAME stream via `-f fixtures/base.dump` succeeds (rc=0) — the
  `-f`/`freopen` path is fine, the bare-stdin path is broken.
- The `load/stdin` golden case records rc=1 + message as the current contract;
  Phase D should decide whether to fix (recommended) or keep.

## Notes
- Severity: D1/D2 are crashers (would benefit from an assert-guard / NULL check
  in the rewrite); D3 breaks the documented stdin mode (man page advertises
  `-f file` default = stdin).
- Suggested owner escalation: fix D3 in Phase D; for D1/D2 decide between fixing
  `chk` cookie handling vs documenting `-i`/`-s` limits.