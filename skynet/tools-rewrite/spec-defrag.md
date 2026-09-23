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

- `mdbx_env_defrag()` drives everything; the only callback is
  `MDBX_defrag_notify_func` (mdbx.h:7692) — progress/result notification
  (moved pages, ratio); there are NO per-page decision hooks in the API,
  and no type named `MDBX_defrag_func`. Verbatim signature (mdbx.h:7783):
  `mdbx_env_defrag(env, defrag_atleast, time_atleast_dot16, defrag_enough,
  time_limit_dot16, acceptable_backlash, preferred_batch, progress_callback,
  ctx, result)`.
- Exclusive open by default; `-c` cooperative.
- Warmup before defrag with `-u`/`-U`.

## Rewrite notes

- `mdbx_env_defrag()` parameter list (mdbx.h:7783, verified — do not trust
  speculative signatures): `(env, defrag_atleast, time_atleast_dot16,
  defrag_enough, time_limit_dot16, acceptable_backlash, preferred_batch,
  progress_callback, ctx, result)`. Confirm the argument mapping at the
  `defrag.c:300-340` call site during Phase B.
- The `MDBX_defrag_notify_func` callback (mdbx.h:7692) translates naturally to
  a `std::function` wrapper.