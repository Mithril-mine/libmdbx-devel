# Spec: mdbx_chk — database integrity checker

Source: `src/tools/chk.c` (663 lines). C API tool; `mdbx_chk` uses `mdbx_env_chk()`.
Man page: `src/man1/mdbx_chk.1`.

## CLI

```
usage: mdbx_chk [-V] [-v] [-q] [-c] [-0|1|2] [-w] [-d] [-i] [-s table] [-u|U] db_pathname
```

getopt optstring (`chk.c:423`): `"uU012TVvqnwctdis:"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | print version banner (`chk.c:440`) and exit 0 |
| `-v` | repeatable | more verbose, up to 9 extra detail levels |
| `-q` | — | quiet |
| `-c` | — | force cooperative mode (don't try exclusive) |
| `-w` | — | write-mode checking |
| `-d` | — | disable page-by-page B-tree traversal |
| `-i` | — | ignore wrong-order errors (custom comparators) |
| `-s table` | yes | process a specific subdatabase only |
| `-u` / `-U` | — | warmup / warmup+lock DB pages before checking |
| `-0`/`-1`/`-2` | — | force using specific meta-page 0/1/2 for checking |
| `-t` | — | turn to a specified meta-page on successful check |
| `-T` | — | turn to a specified meta-page even on unsuccessful check |
| `-n` | — | legacy `MDBX_NOSUBDIR` flag, accepted as a silent no-op (`case 'n': break;`, chk.c:474); environment never opened without subdirectory |

No long options. Usage errors → `usage()` prints to stderr, exits `EXIT_INTERRUPTED`.

## Exit codes (defined `chk.c:46-50`)

| Code | Value | Trigger |
|---|---|---|
| `EXIT_SUCCESS` | 0 | clean check, no problems |
| `EXIT_FAILURE_CHECK_MINOR` | `EXIT_FAILURE` (1) | minor problems found (no db corruption) |
| `EXIT_FAILURE_CHECK_MAJOR` | 2 | major problems / corruption |
| `EXIT_FAILURE_MDBX` | 3 | an mdbx call failed |
| `EXIT_FAILURE_SYS` | 4 | a system/library call failed (clock_gettime etc.) |
| `EXIT_INTERRUPTED` | 5 | interrupted by user break (SIGINT), or usage error |

Mapping at end of main (`chk.c:656-662`): total_problems==0 → SUCCESS; else
`EXIT_FAILURE_CHECK_MAJOR` if `problems_meta || problems_kv || problems_gc`
(or fatal), otherwise `EXIT_FAILURE_CHECK_MINOR`. (There is no `problems_btree`
field; the intermediate counters `tree_problems`/`gc_tree_problems`/
`kv_tree_problems` exist in `MDBX_chk_result` but are not used in the exit map.)

## stdout/stderr contract

- Progress/result messages go to **stdout** via the chk-context print callbacks
  (`print_*` / `MDBX_chk_result_print` style); with `-v` up to 9 extra levels.
- Errors go to **stderr** (`error_fn`/`failure_perror` style:
  `"%s: %s() error %d %s\n"`, `chk.c:64-71`).
- `-q` suppresses non-error stderr chatter.
- The final summary shows problem counts per category (meta/btree/kv/data)
  and a verdict line.

## Behavior notes

- Opens read-write by default (tries exclusive); `-c` forces cooperative mode
  (`chk.c:423-437`, env_flags).
- `-w` enables write-mode checking.
- **Sync-to-disk auto-repair**: `conclude()` (`chk.c:360-377`) — if exactly one
  meta-problem was found and no traversal skip, does `mdbx_env_sync_ex(force)`
  so a steady checkpoint can clear a spurious "recent txn" meta mismatch.
- **Turn-to-meta**: `-t`/`-T` call `mdbx_env_turn_for_recovery()` after a
  successful (or, with `-T`, any) check, only when the DB was opened EXCLUSIVE
  and meta-page `-0/-1/-2` was specified (`chk.c:379-398`).
- `-s table` checks one named sub-DB; otherwise MAIN_DBI.
- Interrupted by SIGINT → `user_break` → `EXIT_INTERRUPTED`.
- Warmup uses `mdbx_env_warmup()` with `MDBX_warmup_force|touchlimit|lock` for `-U`.

## Rewrite notes

- State kept in `static` globals (`chk_flags`, `env_flags`, `only_table`,
  `stuck_meta`, `turn_meta`, `user_break`) — move into a class/struct.
- Signal handling: `user_break` flag set by SIGINT handler.
- `MDBX_chk_context_t` callback struct (lines 300-337) is the natural seam for a
  C++ implementation (lambda adapters or a derived callback object).