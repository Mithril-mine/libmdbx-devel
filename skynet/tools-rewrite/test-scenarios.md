# Test scenarios for Phase B (src/tools → C++)

These are the acceptance-level scenarios the rewritten tools must pass. They are
derived from the current behavior (specs) and existing harnesses.

## S1. Build & smoke
- All 7 tools build from C++ sources on Linux gcc/clang, Windows MSVC/MinGW
  (`-DBUILD_TESTING=ON`, `make tools`), linked against libmdbx C++.
- `mdbx_<tool> -V` prints the version banner and exits 0 on every tool/platform.
- `mdbx_chk -h`/no-args prints usage to stderr and exits with the chk-specific
  code (EXIT_INTERRUPTED); other tools exit 1.
- wingetopt no longer present in `src/tools/` and not in dist amalgamation.

## S2. dump/load round-trip (byte-exactness) — reuse `tests/dump-load.sh`
- Create a DB with: main table (integer + arbitrary keys), one DUPSORT table,
  one named sub-DB, a table with `sequence`, non-zero canary.
- `mdbx_dump -a db > dump.txt`; `mdbx_load -nf dump.txt db2`;
  `mdbx_chk db2` passes; `mdbx_dump -a db2 > dump2.txt`; compare
  `dump.txt == dump2.txt` byte-for-byte (deterministic ordering).
- `-c` concise DUPSORT output: parse back correctly by `mdbx_load`.
- `-p` (print mode) round-trip incl. values containing `\` and high bytes.
- Corrupted-DB rescue: truncate/flip bytes in a page, then
  `mdbx_dump -ar db > out` completes (exit 0) and `mdbx_load -ranf out db2`
  succeeds.
- **Exit-status regression (e55df29d)**: a dump declaring an impossible mapsize
  must make `mdbx_load` exit non-zero (not report success).

## S3. chk
- Clean DB → exit 0, verdict "ok".
- DB with an injected problem (flip a leaf-page checksum) →
  `EXIT_FAILURE_CHECK_MAJOR` (2) with meta/btree/kv breakdown on stdout.
- Minor-only issue → `EXIT_FAILURE_CHECK_MINOR` (1).
- `-0/-1/-2 -t` on a crafted old-meta DB → `mdbx_env_turn_for_recovery`
  invoked; `-T` forces the turn even when the check fails.
- `-s table` checks only a named sub-DB.
- SIGINT mid-check → `EXIT_INTERRUPTED` (5) and DB left consistent.
- The single-meta-problem sync-to-disk auto-repair still works.

## S4. copy
- `mdbx_copy src.db dst.db` → identical env (bytes/geometry); `-c` compact
  produces a smaller file with same content (dump equal).
- `mdbx_copy src.db > out.img` (stdout) byte-equal to the file copy.
- `-f` overwrites an existing target; without `-f` an existing target fails.
- SIGPIPE during stdout copy terminates cleanly (non-zero) without crash.

## S5. defrag
- `-f 100` on a DB with free space reclaims it (size shrinks);
- `-1` single quick cycle; `-t 2` bounds wall time (~2s);
- progress ratio line format unchanged; cooperative `-c` run under a live
  reader still completes.

## S6. drop
- default: empties main DB (entries 0, DB exists);
- `-d`: deletes main DB (open fails after);
- `-s name`: empties/deletes named table only; main DB untouched.

## S7. stat
- `mdbx_stat db` prints the 6-line per-table block with identical column
  alignment to the C version (golden-output compare on a fixed fixture);
- `-e`, `-f`, `-r`(repeatable), `-p`, `-a`, `-s name` each match golden output
  on the same fixture; `-q` suppresses nothing on stdout but silences stderr.

## S8. Exit-code contract parity (all tools)
- Golden matrix: for each tool, list of (invocation, expected exit code) —
  success, usage, missing arg, unreadable DB, corrupt DB — must match the C
  binaries exactly. This is the main CI gate for the rewrite.

## S9. Cross-platform / sanitizers
- Windows MSVC Debug/Release builds run S2-S7 (dump/load round-trip especially).
- ASan/UBSan run of the full scenario suite (via `cmake-asan-build` +
  `tests/dump-load.sh`) reports no leaks/UB in the new parser and common.hpp.

## S10. Regression net
- Full `ctest` (default corpus per TASK-42) + `make check` stays green with the
  rewritten tools substituted.
- `tests/dump-load.sh` and `tests/stochastic.sh` (chk/copy tails) pass.

## S11. CLI help/usage parity (Phase-B checklist)
- Today the tools' own `usage()` strings omit `-n` (load excepted) while all 6
  man pages document it. The rewrite should reconcile `--help` output with the
  man pages — new usage/help must list every accepted flag (`-n` included).