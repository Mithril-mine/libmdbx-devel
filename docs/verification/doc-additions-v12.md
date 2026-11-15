# Doc additions — tranche V12 (stream A: `src/tools/` — the CLI shells over untrusted data)

Scope: statement-level verification of `src/tools/{chk,copy,drop,dump,load,stat}.c`,
`src/tools/common.h`, `src/tools/wingetopt.{c,h}` (650+131+163+533+964+433+150+87+26
lines, all read fully; every function ledgered in the tranche report). The tools are
the crafted-data front line together with `mdbx_chk`: they parse **untrusted** dump
streams (`mdbx_load`) and are expected to fail cleanly on adversarial input (trust
model A7). The chk *engine* (`src/chk*.c`) is another tranche; here only the CLI
shell (`src/tools/chk.c`) is covered.

Machine evidence (all on the verification tree, default and
`-fsanitize=address,undefined -fno-sanitize=alignment` builds):
- dump→load round trips **byte-identical** for `-a` normal, `-c` (concise) and
  `-p` (printable) modes, including DUPSORT repeats, empty values, values with
  control/high-bit/backslash/leading-space bytes, named tables plus raw MAIN
  records (`#385` coverage) and the trailing MAIN section.
- `#70` write-error propagation: `mdbx_dump > /dev/full` exits 1 (final
  `fflush`+`ferror` check, dump.c:500-508).
- Fuzz: 600 mutated/malformed dumps × {`load`, `load -r -p`, `dump -a`, `chk`,
  `stat -a -f -e`, `drop`} under ASAN+UBSAN — **0 crashes, 0 tool-level UB**.
  (The `-fsanitize-undefined-trap-on-error` build traps inside the *library's*
  intentional unaligned accesses, `src/unaligned.h` under `MDBX_UNALIGNED_OK`,
  i.e. engine scope and a UBSAN false positive on x86 — exclude alignment when
  fuzzing tools.)
- wingetopt + `tool_help_requested` unit driver: cluster/arg/`--`/`-`/illegal/
  missing-arg semantics all POSIX-conformant; `-s --help` / `-s -h` consumption
  honored; the "help requested after a positional" quirk confirmed (F-V12-10).
- copy: content-identical copy (`-n`), compact copy (`-c`) passes `mdbx_chk`,
  overwrite refused without `-f`, copy to `/dev/full` exits 1.
- chk exit codes: healthy 0, `-s` unknown table 3 (`EXIT_FAILURE_MDBX`),
  truncated-file 4 (`EXIT_FAILURE_SYS`), no signals/crashes.

Verdicts: **PROVED** = statement-level proof; **PROVED-A** = holds with labeled
assumptions; **SUSPECT/REFUTED** as per protocol (no REFUTED this tranche).

---

## 1. Tool contracts (as verified; add to man-page knowledge)

### mdbx_dump ↔ mdbx_load stream format (INV-T1, INV-T2)

- **Section partition is exact** (the `#385` fix, dump.c:377-495). `mdbx_dump -a`
  iterates MAIN's records with `cursor_get(..., nullptr, MDBX_NEXT_NODUP)` (null
  `data` is legal for NODUP ops, `cursor-nav.c` uses `get_key_optional`); keys
  with an embedded NUL are never table names and are left for the trailing raw
  section; each remaining key is `dbi_open_ex(..., MDBX_DB_ACCEDE)`-probed:
  success → its own `database=<name>` section (record added to the skip list),
  `MDBX_INCOMPATIBLE` → raw MAIN data (`have_raw`), other error → clean failure
  (non-rescue) or skip+continue (rescue). The trailing MAIN section carries no
  `database=` line and skips exactly the successfully-opened table names.
  Therefore every MAIN record appears exactly once (rescue-mode records whose
  probe failed with a non-INCOMPATIBLE error may appear *twice*: skipped-nothing
  in the raw section after a failed section — rescue-only, tolerated). Machine
  checked: round trip byte-identical.
- **GLOBAL header once**: `mode -= GLOBAL` (dump.c:91) prints
  `geometry/mapsize/canary` in the first emitted section only; `mdbx_load`
  ignores geometry-family lines outside the first header (load.c, "non-global
  context" ignores). Consistent pair.
- **Concise mode is a line-split marker** (INV-T2): a DUPSORT dup is emitted as
  `"  <hex>"` — dump.c:177 `putchar(' ')` + `dumpval` (leading space). On load,
  the *key* readline consumes the first space, sees the second space → concise
  key-repeat (returns SUCCESS leaving the previous key), pushes the second space
  back, and the *value* readline then reads the remainder of the same physical
  line. A second-space marker at a *value* position is rejected
  (`concise-repeat marker for a value`, ENODATA). Hand-written dumps must follow
  this; `dump -c` output does.
- **Trailer discipline (`#70`)**: `DATA=END` is printed only when the record
  loop reached clean end (`MDBX_NOTFOUND`) or in rescue mode with a non-EINTR
  error (dump.c:184-186); an interrupted dump omits the trailer. On load, a
  missing trailer at key position or EOF mid-header ⇒ `MDBX_ENODATA` and
  failure; EOF at *value* position is reported as "failed to read value"
  (message-quality note, F-V12-9).
- **Write errors**: dump checks `fflush(stdout); ferror(stdout)` after all
  output and converts to `MDBX_EIO` failure; with `-f file` the freopen happens
  only after the command line is fully validated (no accidental truncation on
  mistyped invocations).
- **Escaping**: `dumpval` hex-encodes any byte that is not `isprint`, not
  backslash, and not a non-leading space; a *leading* space is escaped
  (`\20`) because the stream grammar prefixes data lines with one space. The
  encoder `'0'+d+(((9-d)>>7)&39)` is correct for `d∈[0,15]` (verified by
  evaluation); `unhex` is the exact inverse and is only called behind
  `isxdigit` guards. The escape decoder reads `r[1]` at most up to the written
  NUL sentinel and `r[2]` only when `r+3<=end`; writes are in-place with
  `w<=r`; growth loops double the buffer with correct `fgets` bounds.
  Page-size independence: no comparisons of psize or derived values against
  constants anywhere in the tools (AGENTS §7 rule holds); the only page-size
  arithmetic is user-input range checks against `MDBX_MIN/MAX_PAGESIZE`.

### mdbx_load contracts

- Geometry/mapsize/pagesize/maxreaders from the header are range-checked
  (`MIN_MAPSIZE..MAX_MAPSIZE64`, `MDBX_READERS_LIMIT`, pagesize
  `MDBX_MIN/MAX_PAGESIZE`) and then delegated to `mdbx_env_set_geometry`
  (`#312`); invalid values are *ignored* with a warning, not fatal — except
  `VERSION≠3`, `type≠btree`, `format≠{print,bytevalue}`, bad `geometry`/`canary`
  syntax and control characters in table names, which are fatal.
- `-s name` takes priority over `database=`; an empty `database=` value names
  the empty-string table (machine-checked accepted).
- Sequence (`#312` family): `present > header` refuses (warning + failure);
  `present < header` advances by the delta. Verified.
- One txn per section; sections commit independently (partial load on a later
  failure is by design); batch checkpointing (`-b`, `-L`) re-binds the cursor
  after `mdbx_txn_checkpoint`; `-N` skips KEYEXIST; rescue skips
  `MDBX_BAD_VALSIZE` (oversize keys machine-checked both ways).
- `-f file` fallback: with no positional left, the `-f` argument is the dbpath
  and input stays stdin (man-page promise, load.c:625-638).
- `-p` purge: **incompatible with multi-table dumps** (F-V12-2 below).

### mdbx_chk shell contracts

- Exit code map: 0 ok; 1 `EXIT_FAILURE_CHECK_MINOR`; 2
  `EXIT_FAILURE_CHECK_MAJOR` (meta/kv/gc problems); 3 `EXIT_FAILURE_MDBX`;
  4 `EXIT_FAILURE_SYS`; 5 `EXIT_INTERRUPTED` (also used by `usage()`).
  Signals during checking produce exit 3 (not 5) because all mdbx errors are
  negative — F-V12-8.
- `-s table` that matches nothing fails (`table_matched` via the
  `table_filter` callback) — an unknown target cannot silently "check nothing".
- `-t`/`-T` (meta turn) validation in `main` makes `conclude()`'s EXCLUSIVE
  gate unreachable-false: `-t` ⇒ `-0/-1/-2` ⇒ EXCLUSIVE enforced ⇒ no silent
  skip path.
- Auto-fix path: exactly one meta problem, full traversal, write mode, no
  `-s`, no stuck meta, `steady < recent` ⇒ sync-to-disk and decrement both
  counters (`conclude`, chk.c:359-400).
- `logger()` counts lib ERROR/FATAL as chk problems (`mdbx_env_chk_encount_problem`).

### mdbx_copy / mdbx_drop / mdbx_stat contracts

- copy: manual single-char option parsing (no clustering, no `--`, unknown
  option silently usage-errors); banner goes to stderr when dest is stdout and
  to stdout otherwise; `MDBX_CP_OVERWRITE` via `-f`; content-identical copy and
  compact copy machine-checked.
- drop: default (no `-s`) empties MAIN; **`mdbx_drop` refuses MAIN while it
  holds any named-table records** (API guard, api-dbi.c `named_table_probe` ⇒
  `MDBX_INCOMPATIBLE`) — named tables must be dropped explicitly first. Clean
  error, no corruption (machine-checked). Usage text does not mention this.
- stat: `-a`/`-s` mutually exclusive; `-a` uses `mdbx_enumerate_tables`
  (temp self-looped cursor, no handle opens — `maxdbs=2` is sufficient; the
  same reasoning covers the `maxdbs=2` pattern in dump/drop/load since
  `mdbx_dbi_close` releases the slot and table handles are opened/closed
  sequentially); gc arithmetic (`pages_total - pages_allocated` etc.) trusts
  the `mdbx_gc_info` invariants (PROVED-A, engine scope); percent/size
  renderers are division-guarded (`value && value < bound`) and use 42-byte
  buffers = exactly `sizeof(ratio2digits_buffer_t)`.
- All tools install break handlers; chk/dump/load poll `user_break`; **copy and
  drop never poll** (F-V12-7).

---

## 2. Findings

| id | severity | file:lines | one-line |
|---|---|---|---|
| F-V12-1 | P3 data-integrity | load.c:398-427, 146-159 | NUL byte silently truncates a parsed line; tail bytes dropped |
| F-V12-2 | P2 functional | load.c:787-797 | `-p` purge fails (`MDBX_INCOMPATIBLE`) on every multi-table dump, leaves partial load |
| F-V12-3 | P7 exit-code | stat.c:310-334, 432 | `-r` exits FAILURE when reader table absent / `-rr` when cleanup empties it |
| F-V12-4 | P3 contract | drop.c:149-158 | abort on txn handle already terminated by failed commit (documented-invalid handle) |
| F-V12-5 | P4 hardening | chk.c:172-173, common.h:146 | tool loggers call vfprintf without `fmt==nullptr` guard (gcc -Wformat-overflow fires at -O1) |
| F-V12-6 | P7 CLI | copy.c:65-88 | hand-rolled options: no clustering/`--`, unknown option silently → usage |
| F-V12-7 | P3 UX | copy.c, drop.c | break handlers installed but `user_break` never polled — SIGINT/SIGTERM swallowed |
| F-V12-8 | P4 exit-code | chk.c:620-624 | `EXIT_INTERRUPTED` unreachable on signals (negative rc ⇒ exit 3); usage exits 5 |
| F-V12-9 | P4 message | load.c:850-854 | value-readline EOF reported as "failed to read value"; PRIiSIZE for unsigned lineno |
| F-V12-10 | P4 cosmetic | common.h:58-90 | help detection scans past positionals (`db -h` prints help where getopt would usage-error) |
| F-V12-11 | INFO doc | drop.c + usage | default "empty the main DB" refuses while named tables exist (API guard) — undocumented |

**F-V12-1 counterexample (machine-verified)**: header + ` 616263\n 313233\n` +
` \x00ZZJUNKTAIL\n` + ` 353535\n` + `DATA=END\n` loads with exit 0 producing
record `("" → "555")`; the bytes `ZZJUNKTAIL` are consumed and silently
discarded. `readline` sets `line[0]=0`, `strlen` yields 0, the growth/strip and
decode loops are skipped, `out->iov_len=0`. Any other invalid byte would
`badend()`. Same class: `database=na\0me` silently creates table `na`. Fix:
`memchr(line, '\0', len)` ⇒ `badend()` after `fgets` in both `readline` and
`readhdr`.

**F-V12-2 counterexample (machine-verified, instrumented run)**:
`mdbx_load -p fresh.db < dump-a-output` → purge(alpha) ok, purge(dups) ok,
then the trailing MAIN section: `purge(MAIN)` → `MDBX_INCOMPATIBLE` (-30784)
because MAIN still holds the `alpha`/`dups` N_TREE records (`mdbx_drop` refuses
MAIN with named tables, api-dbi.c `named_table_probe`). Whole load exits 1;
sections processed earlier are already committed (one txn per section) — the
destination is left partially loaded. Every `mdbx_dump -a` dump with ≥1 named
table is affected (named sections always precede the trailing MAIN section).
Works: `-p` with `-s` (single table, purge-once), table-less dumps, fresh
non-`-a` dumps. Fix directions: document `-p` as single-table-only, or purge
the raw remainder of MAIN instead of `mdbx_drop`, or drop the named tables'
MAIN records as part of `-p`.

**F-V12-3**: `rc == MDBX_RESULT_TRUE` (-1) after "Reader Table is absent" (and
after the `-rr` "Now Reader Table is empty" path) flows unchanged to
`return rc ? EXIT_FAILURE : EXIT_SUCCESS`. Reachability of the absent path
requires a lckless env (read-only mount); code-level control-flow proof.

**F-V12-4**: per mdbx.h:4592-4599, any `mdbx_txn_commit` result other than
`MDBX_THREAD_MISMATCH` means the handle is terminated/invalid; drop.c then
calls `mdbx_txn_abort(txn)` on it. In the current implementation the basal-txn
memory is retained and `check_txn` returns `MDBX_BAD_TXN` — memory-safe today,
contract violation nonetheless; load.c:912-913 shows the correct pattern
(`txn = nullptr` immediately).

**Related hardening (fold into the F-V12-5 fix)**: load.c's `readline` decode
loops call `isxdigit(r[1])`/`isxdigit(r[2])` on plain `char` — negative for
bytes ≥ 0x80 on signed-char platforms, which is UB per the C standard (the
argument must be an `unsigned char` value or EOF); benign on glibc/musl/MSVC
tables. dump.c does it correctly (`const unsigned char *c` feeds `isprint`).
Cast the ctype arguments in load.c: in bytevalue mode every input byte reaches
`isxdigit`, so crafted high-bit input hits this at every odd position.

---

## 3. Established invariants / lemmas (KB-synced)

- **INV-T1 (dump-a partition exactness)**: see §1; with the rescue-mode caveat
  (dup raw appearance) and the NUL-key ⇒ raw rule. Machine-checked.
- **INV-T2 (concise grammar)**: dup lines are `"  <value>"`; loader splits the
  physical line across the key/value readlines; a value-position marker is
  fatal. Round trip machine-checked (`dump -a -c` → `load` → `dump -a`
  byte-identical).
- **LEM-V12-hex**: `dumpval`'s branchless hex encoder and load's `unhex` are
  exact inverses on `isxdigit`-guarded input; the `(9-d)>>7` trick relies on
  arithmetic shift of a negative int (implementation-defined pre-C23; the
  compilers used for tools all define it).
- **LEM-V12-readline-bounds**: escape decode: `r[1]` reads touch at most the
  NUL sentinel; `r[2]` requires `r+3<=end`; `w<=r`; buffer growth always
  `fgets(base+len, newsize-len)` with `len < oldsize <= newsize`. No OOB.
- **LEM-V12-maxdbs2**: `mdbx_env_set_maxdbs(env, 2)` + strictly sequential
  open/close of at most one named handle suffices for any number of named
  tables (`dbi_close_locked` frees the slot) — covers dump `-a`, drop `-s`,
  load sections, stat `-a`.
- **LEM-V12-commit-contract**: per mdbx.h, commit ≠ THREAD_MISMATCH ⇒ handle
  invalid; basal implementation retains handle memory so late abort is a
  guarded error return (see F-V12-4).
- **FUZZ-V12**: 600 mutated dumps × 6 tools under ASAN+UBSAN (alignment
  excluded) — clean. Corpus generator + runner preserved in the tranche
  machine-check scripts (regenerable; see §4).

---

## 4. Test-coverage gaps (proposed fuzz/CI surface per tool)

- **mdbx_load** (the parser is the biggest crafted-input surface):
  - fuzz line-parser with *structured* mutations (flip bytes inside hex/print
    escapes, insert NUL/backspace/high-bit, truncate at every offset of a valid
    dump — truncation-at-offset-k sweep, not only EOF cases);
  - shapes not in this tranche's corpus: keys > maxkeysize with `-a` (append +
    `equal_or_greater`), `-N` no-overwrite conflicts, `-b/-L` checkpoint
    re-bind paths under small `-L`, `-G` boundary geometries (shrink>upper etc.
    — API rejects, tool must surface), CRLF and UTF-8 names, section header
    repetitions, `sequence` at `UINT64_MAX`, concurrent reader during load
    (MAP_FULL commit failure → F-V12-4 path);
  - property test: `dump -a [−c|−p] → load → dump -a` byte-identity over
    randomized DB content (this tranche did fixed content; make it property-
    based with random table names incl. empty-name and 64K names).
- **mdbx_dump**: corrupt-DB corpus under `-r` (rescue) — needs the chk-engine's
  crafted-DB generator; `-f` target write failures (ENOSPC via small fs/tmpfs
  limit); page-size sweep 256..65536 (dump/load round trip per pagesize —
  AGENTS §7: never vs constants, only round trips); a DB with a named table
  whose name equals a raw record key of another table (skip-list collision
  impossibility, statement-proven, cheap to lock with a test).
- **mdbx_chk** (shell only): `-w -c` combos, `-t/-T` + wrong `stuck_meta`,
  `-s` filter with mistyped names, `-u/-U` warmup on RDONLY, interruption
  (SIGINT) mid-check ⇒ exit 3 (document F-V12-8), `verbosity` 0..9 clamps.
- **mdbx_copy**: `SIGPIPE`/EPIPE on stdout dest; dest on different filesystem;
  huge-geometry source (TOO_LARGE path machine-hit here under ASAN VA limits —
  worth an explicit test with `upper > map-limit` expecting clean MDBX_TOO_LARGE);
  `-d` dynamic-size, `-p` throttle with a concurrent reader.
- **mdbx_drop**: `-d` on a table with a concurrent reader (commit vs snapshot);
  drop MAIN of table-less DB (success) vs table-ful (refused) — lock INV;
  double drop of same name.
- **mdbx_stat**: `-r` exit codes (F-V12-3 — needs a lckless fixture: tmpfs
  mounted `ro` loopback or `MDBX_NOSUBDIR` + read-only file perms);
  `-ff` histogram rendering with crafted GC; reader list with parked/ousted
  thread sentinels (`MDBX_TID_TXN_OUSTED/PARKED` rendering).
- **wingetopt** (windows-only): port the /tmp unit driver (cluster, `-Xarg`,
  `-X arg`, missing-arg, illegal, `--`, `-`, repeated `getopt` with `optind`
  reset) into a Windows CI job; on Linux the tools use libc getopt, so
  wingetopt regressions are invisible to Linux CI today.

---

## 5. Verdict distribution (per-function ledger in the tranche report)

PROVED: 61 functions/statements-groups; PROVED-A: 9 (rescue-mode hacks, gc-info
arithmetic trust, append comparator semantics, ratio-buffer exactness, txn-reset
harness, wingetopt statics, NUL-locale isprint, asan-va copy artifact,
chk line-count phantom offset); REFUTED: 0; SUSPECT: 0. Findings: 11 (1×P2,
3×P3, 5×P4/P7, 2×INFO), no memory-safety violation found in tool code.
