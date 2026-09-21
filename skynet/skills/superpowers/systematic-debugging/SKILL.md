---
name: systematic-debugging
description: Use when something is broken or behaves unexpectedly in libmdbx, to find and fix the root cause through a 4-phase process instead of guessing
---

# Systematic Debugging

*Adapted from obra/superpowers (MIT). This engine fails in intricate ways (MVCC, CoW,
locking, TLS); guessing wastes hours. Follow the four phases.*

## Phase 1 — Reproduce & characterize

- Get a **minimal, deterministic reproduction** if possible:
  - Regression test in `tests/issues/issue_ghNNNN.c++` (TDD red).
  - For stochastic failures: capture the `--prng-seed` (the script seeds from `date+%s+RANDOM`)
    and rerun `mdbx_test` with that seed; narrow via `--nops`, `--pagesize`, `--mode`,
    `--table`.
- Record the exact symptom: error code, assertion/panic message (`panic_at` prints function +
  line), ASAN/UBSAN report, `mdbx_chk` complaint.
- Determine which layer it belongs to: public API → `api-*.c`; transaction mechanics →
  `txn*.c`/`mvcc-readers`/`rthc`; B-tree → `node/dml/dpl/tree-*/page-*`; persistence →
  `meta/dxb/gc`; OS → `osal/lck`.

## Phase 2 — Hypothesize the root cause

Form hypotheses from the invariants in [`../../../architecture.md`](../../../architecture.md) §8, e.g.:

- Page state machine (`frozen/spilled/shadowed/modifiable/tmp`) violated?
- Two-phase meta update reordered?
- GC handed out a page still visible to a reader?
- Reader slot not cleaned on thread exit (TLS destructor contract)?
- Locking flavor specific (`MDBX_LOCKING` SYSV/1988/2001/2008)?

One hypothesis at a time. Make it falsifiable.

## Phase 3 — Prove it

- Instrument minimally: build with `MDBX_CHECKING=2` + `MDBX_FORCE_ASSERTIONS=1` (CI defaults)
  to turn invariant violations into immediate panics with locations.
- Use sanitizers: `make test-asan`, `make test-ubsan`, `make test-memcheck` (valgrind with
  `valgrind.supp` suppressions).
- `MDBX_DEBUG` logging levels + `--loglevel` in `mdbx_test`; `TRACE/DEBUG/VERBOSE` markers.
- Bisect: git history, or binary search across `--from/--upto` nops ranges, or toggle
  `--mode` flags (`+/-writemap, +/-lifo, ...`) to isolate.
- Confirm the root cause before touching code. If the evidence contradicts the hypothesis,
  discard it and form a new one.

## Phase 4 — Fix & verify

- Fix via TDD: failing regression first, then minimal change (see `test-driven-development`).
- Verify: the new regression passes; **the whole family** passes (`ctest`); sanitizers clean;
  `make smoke`; for concurrency/GC/meta bugs run `make test-stochastic`.
- Long soak is **optional and human-driven**: `tests/battery-tmux.sh` runs *indefinitely* in
  tmux until a human stops it — typical use is hours/days of thorough verification on a strong
  machine. It is **not** suitable for automated testing/CI; do not treat it as a completion gate.
- Document: reference the issue number; explain the root cause in the commit message and
  optionally in the regression test comment.

## Anti-patterns

- Changing random things until it "looks fixed" (no root cause).
- Fixing the symptom (e.g., suppressing an assert) instead of the cause.
- Ignoring "cannot happen" paths — in this engine they happen (see `tests/exploits/`).
- Skipping the failing-test proof (a fix that cannot be regression-tested is unverified).

## Project notes (libmdbx)

- Key debug entry points: `src/logging_and_debug.h` (panic/ENSURE/CHECKS0-2), `src/chk.c`
  (structure validation), `tests/.gdbinit` + `tests/with.gdb` (GDB under `stochastic.sh
  --with-gdb`), `mdbx_chk -vvn[w]` (repair mode `-w`).
- Known-hard areas (from history): incoherent unified page cache (#269, `coherency.c`), glibc
  TLS destructor bugs (#21031/#21032, `rthc.c`), reader lag vs GC (`mvcc_kick_laggards`),
  spilling vs cursor tracking (`PROBE_AGAINST_DANGLING_*` in `proto.h`).
- Fuzzing-like PoCs live in `tests/exploits/`; new findings should get a PoC + regression.