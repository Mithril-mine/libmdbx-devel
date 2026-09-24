# Spec: mdbx_load — load dump stream into a DB

Source: `src/tools/load.c` (906 lines). Man page: `src/man1/mdbx_load.1`.

## CLI

```
usage: mdbx_load [-V] [-q] [-a] [-f file] [-s name] [-N] [-p] [-T] [-r] [-n] dbpath
```

getopt optstring (`load.c:506`): `"ab:L:d:G:f:ns:NTpVrq"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | version banner, exit 0 |
| `-q` | — | quiet |
| `-a` | — | append records in input order (required for custom comparators) |
| `-b number` | yes | insertion batch size in items (default 100K) |
| `-L megabytes` | yes | limit transaction amount (in MB) |
| `-d percent` | yes | desired page fill density 50..100 (default 100) |
| `-G L:U:G:S:P` | yes | override geometry (5 colon-separated numbers, see `mdbx_env_set_geometry`) |
| `-f file` | yes | read from file instead of stdin |
| `-s name` | yes | load into named table |
| `-N` | — | don't overwrite existing records (skip them) |
| `-p` | — | purge target table(s) before loading |
| `-T` | — | read plaintext |
| `-r` | — | rescue mode (ignore errors in corrupted dump) |
| `-n` | — | `MDBX_NOSUBDIR` (no subdirectory for new DB) |

## Input format

Same BDB-compatible stream as produced by `mdbx_dump`. Header parsing is a
strict-ish state machine (`readhdr`, `load.c:142-...`):

- `VERSION=3` — must be 3, else fatal (`load.c:161-168`).
- `db_pagesize`, `mapsize`, `maxreaders`, `txnid`, `mapaddr` — validated;
  out-of-range or non-global values are **ignored with a warning**
  (`load.c:171-269`).
- `format=print|bytevalue` — selects plaintext vs hex parse mode.
- `database=<name>` — sets target sub-DB name (`load.c:200-212`).
- `type=btree` — only btree accepted (`load.c:214-223`).
- `canary=...`, `sequence=...` — restored.
- `HEADER=END` ends the header; items follow; `DATA=END` ends a table.
- Multiple tables in one stream (via repeated headers) are supported.

Item parse: hex pairs (bytevalue) or literal chars (print); single leading
space; lines split key/data; DUPSORT values appended per line.

## Exit codes

- 0 success; 1 (`EXIT_FAILURE`) on any error.
- **e55df29d fix** (`load.c:675-707` area): mapsize and maxkeysize failures now
  set a real `err` (`MDBX_TOO_LARGE` / `MDBX_PROBLEM`) before `goto bailout`, so
  the tool exits non-zero instead of reporting success — do not regress this.
- Malformed header lines → stderr message + `exit(EXIT_FAILURE)`.

## Behavior notes

- stdin by default; `-f file` reads a file.
- **Issue #50 fix (2026-09-24)**: the input-file `freopen` is deferred until
  after the dbpath is identified. If `-f` consumed the only remaining argument
  (no positional left), that argument is treated as the dbpath and stdin is
  used — so `mdbx_load -nf <db>` reads the dump from stdin into `<db>`,
  matching the man-page default.
- Batching: `-b` items per txn, `-L` MB cap; `mdbx_txn_checkpoint`/info used to
  manage durability vs speed.
- `-p` purges the table before loading (empty/delete).
- `-N` uses `MDBX_NODUPDATA`-like skip semantics.
- Cursor-based inserts: `mdbx_cursor_bind`, `mdbx_cursor_put` (with
  `MDBX_APPEND`-style optimization when input is sorted — verify at call site).
- `-a` preserves input order (for custom comparators).
- `mdbx_dbi_open_ex` with ACCEDE/CREATE; `mdbx_env_set_maxdbs(env, 2)` when
  named tables are used; `mdbx_env_set_maxreaders` and `mdbx_env_set_option`
  applied from stream/CLI.
- SIGINT → `user_break` → graceful abort (non-zero exit).

## Rewrite notes

- Largest tool (906 lines); the readhdr state machine and the line parser are
  the tricky parts. A table-driven parser (struct per header key) plus a small
  line/buffer abstraction is recommended.
- The `-T` plaintext and `-r` rescue paths interact with the parse mode flags
  (`mode & PLAINTEXT`, `& NOHDR`, `& GLOBAL`, `load.c:120-123`) — preserve
  these semantics.