# Spec: mdbx_copy — environment copier

Source: `src/tools/copy.c` (166 lines). Man page: `src/man1/mdbx_copy.1`.

## CLI

```
usage: mdbx_copy [-V] [-q] [-c] [-d] [-f] [-p] [-n] [-u|U] src_path [dest_path]
```

**Notable:** copy.c does NOT use getopt — it hand-parses `argv[1]` short flags in
a loop (`copy.c:79-112`). Only single-letter `-x` forms (no `--long` except
`--help`, no `-abc` bundling beyond single chars, no `-h`? — `-h`/`--help`
both print usage).

| Opt | Meaning |
|---|---|
| `-V` | print version banner, exit 0 |
| `-q` | quiet |
| `-c` | compact copy (`MDBX_CP_COMPACT`, skip unused pages) |
| `-d` | force dynamic-size DB (`MDBX_CP_FORCE_DYNAMIC_SIZE`) |
| `-f` | force overwrite of existing target (`MDBX_CP_OVERWRITE`) |
| `-p` | throttle MVCC during copy (`MDBX_CP_THROTTLE_MVCC`) |
| `-n` | source opened with `MDBX_NOSUBDIR` |
| `-u` / `-U` | warmup / warmup+lock before copying |
| `-h` / `--help` | usage |

Positional: `src_path` (required), `dest_path` (optional; **stdout** if omitted →
copy via `mdbx_env_copy2fd()`). `argc` must be 2..3 (`copy.c:114`).

## Exit codes

- 0 on success (`copy.c:165`); 1 (`EXIT_FAILURE`) on any error or usage.
- SIGPIPE/SIGHUP handled (`signal_handler`, `copy.c:120-125`): a broken pipe
  aborts the copy gracefully.

## stdout/stderr contract

- Version banner to stdout (`-V`).
- Copy progress/result is minimal; errors to stderr
  (`"%s: %s() error %d %s\n"`, logger prefixes to stderr).
- With `dest_path` omitted, the DB bytes stream to stdout (binary); `-q` silences chatter.

## Behavior notes

- Source opened read-only; `-n` adds `MDBX_NOSUBDIR`.
- Compact copy via `MDBX_CP_COMPACT` requires... (see code: `mdbx_env_copy`
  vs `mdbx_env_copy2fd`; `mdbx_env_copy` used when dest is a path,
  `mdbx_env_copy2fd` when writing to stdout).
- Warmup: `mdbx_env_warmup(env, nullptr, warmup_flags, 3600*65536)`.
- Windows: `SetConsoleCtrlHandler(ConsoleBreakHandlerRoutine, true)`.

## Rewrite notes

- Only tool with a bespoke arg parser — a uniform LLVM-style parser would
  replace it (see design.md); must keep `-n -c -d -p -f -q -u -U` semantics and
  the `src [dest]` positional convention.
- Binary-to-stdout path must remain byte-exact.