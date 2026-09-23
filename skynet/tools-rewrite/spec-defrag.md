# Spec: mdbx_defrag — database defragmenter

Source: `src/tools/defrag.c` (429 lines). **No man page** (absent from `MANPAGES`,
GNUmakefile:152).

## CLI

```
usage: mdbx_defrag [-V] [-v[v[v...]]] [-q] [-1..9] [-t seconds] [-f percent]
                   [-r percent] [-s megabytes] [-c] [-u|U] db_pathname
```

getopt optstring (`defrag.c:193`): `"Vvq123456789s:t:f:r:cuU"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | version banner, exit 0 |
| `-v` | repeatable | verbose (extra detail from debug builds) |
| `-q` | — | quiet |
| `-1` | — | single quick defrag cycle without two-stage moves |
| `-2..-9` | — | limit the number of defrag cycles |
| `-t seconds` | yes | limit duration in seconds |
| `-f percent` | yes | free up given % of unused space (default 100) |
| `-r percent` | yes | acceptable undefragmented residue % (default 0) |
| `-s megabytes` | yes | preferred defrag step/transaction size |
| `-c` | — | force cooperative mode (no exclusive) |
| `-u` / `-U` | — | warmup / warmup+lock before defragmenting |

## Exit codes

- 0 on success, 1 (`EXIT_FAILURE`) on any error/usage (`defrag.c:216,428`).
- Usage/`-V` handling: usage() exits 1.

## stdout/stderr contract

- Progress ("+" pages moved, etc.) to stdout; ratio via
  `mdbx_ratio2digits()`.
- Errors to stderr via logger prefixes (`"!!!fatal:"`, `" ! "`, ...).
- `-q` suppresses progress chatter.

## Behavior notes

- `mdbx_env_defrag()` drives everything; callbacks:
  - `MDBX_defrag_func` for page move/preserve/remove (per-page decision hooks),
  - `progress_func` reports moved bytes and percent (ratio display),
  - `timeout` via `-t seconds`,
  - ratio target via `-f percent` (free-space goal) and `-r percent` (residue).
- Exclusive open by default; `-c` cooperative.
- Warmup before defrag with `-u`/`-U`.

## Rewrite notes

- `mdbx_env_defrag()` signature: `(env, func, ctx, timeout, ratio, retry_limit,
  progress, page_size, ...)` — verify exact parameter list at `defrag.c:300-340`
  call site during Phase B.
- Callback design translates naturally to `std::function` wrappers.