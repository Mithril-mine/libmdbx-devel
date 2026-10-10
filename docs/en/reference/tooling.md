# Tooling: command-line utilities

> Related: [First steps](../getting-started/first-steps.md) ·
> [Durability modes](../getting-started/durability-modes.md) ·
> [API reference](../reference/index.md)

The amalgamated package includes utilities for database administration and
diagnostics (build with `MDBX_BUILD_TOOLS=ON` or `make mdbx_chk` etc.).

## mdbx_chk — integrity check

Validates the database structure: the B+tree, GC consistency, key ordering,
page accounting. Recommended:

- after crashes while debugging your own integration;
- periodically — as part of operational monitoring;
- after restoring from a backup.

```sh
mdbx_chk database.mdb          # basic check
mdbx_chk -vvn database.mdb     # verbose, no modifications
mdbx_chk -w database.mdb       # write mode (reconcilable fixes)
```

Details: `mdbx_chk -h`. Key options: `-v` (verbosity 0..9), `-n` (no-modify),
`-w` (write mode).

## mdbx_copy — hot copying

A consistent copy of the database **without stopping writers** — an ordered
page walk over the snapshot. A plain file copy (`cp`) during active writes
gives no such guarantee.

```sh
mdbx_copy source.mdb backup.mdb
mdbx_copy -c source.mdb out.tar.gz   # a compressed copy
```

The programmatic counterpart — `mdbx_env_copy2...()` (the C API).

## mdbx_dump / mdbx_load — dump and load

A text (or unicode-escaped) dump of the database content → migration between
platforms, content diagnostics, recovery:

```sh
mdbx_dump database.mdb > dump.txt
mdbx_load -n new.mdb < dump.txt   # -n: no locking at creation
```

## mdbx_stat — statistics

Statistics of the environment, tables, transactions and readers: sizes,
page/item counters, the enumeration of active readers (useful when
diagnosing "long-lived readers" — see
[restrictions](../overview/restrictions.md)).

```sh
mdbx_stat database.mdb
mdbx_stat -a -nn database.mdb
```

## mdbx_defrag — defragmentation

Moves live pages towards the file start and truncates the tail. Usually not
required (the continuous zero-overhead compactification works on every
commit), but useful after massive deletions in older database versions.

```sh
mdbx_defrag database.mdb
```

## General recommendations

- All the utilities are safe to run against a **copy** for diagnostics.
- `mdbx_chk -V` shows the build options (`options:` string) — with
  container/cross-host usage the string must match (see
  [Installation and building](../getting-started/install-build.md#containers)).
- For automation — the exit codes reflect the operation status; the verbose
  output is controlled by the verbosity options.
