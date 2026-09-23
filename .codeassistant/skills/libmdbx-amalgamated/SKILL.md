---
name: libmdbx-amalgamated
description: >
  Work with the amalgamated (single-file) libmdbx embedded key-value database:
  build it, embed it into a C/C++ project, use the C or C++ API, diagnose
  failures, and answer user questions about libmdbx behavior.
license: MIT
---

# libmdbx — Amalgamated Version Guide

libmdbx (aka MDBX) is an extremely fast, compact, powerful, embedded,
transactional key-value storage engine, a deeply revised descendant of LMDB.
This skill covers the **amalgamated distribution** — the self-contained
single-file form intended for embedding and packaging. The dev repository
(tests, internals documentation) is separate and not shipped here.

## Sources & docs

- Home / documentation / doxygen: <https://libmdbx.dqdkfa.ru>
- Amalgamated source: <https://sourcecraft.dev/dqdkfa/libmdbx>, mirror
  <https://github.com/Mithril-mine/libmdbx>
- Release tarball contains only the files listed under "Distribution layout"
  below. In-repo files like `tests/`, `skynet/`, `AGENTS.md` are **not** part
  of the amalgamated version.
- **`AGENTS-GUIDE.md`** (ships in the tarball): architecture summary, correct
  usage patterns, safe-work rules (risks/anti-patterns) and a self-contained
  checklist for external agents — read it before doing any work on the DB.

## Distribution layout (what the user actually has)

Core sources (`mdbx.c` is the whole engine; `mdbx.h` the C API;
`mdbx.h++`/`mdbx.c++` the optional C++ API):

```
mdbx.c  mdbx.h  mdbx.h++  mdbx.c++  mdbx-internals.h  mdbx-wingetopt.h
mdbx_chk.c  mdbx_dump.c  mdbx_load.c  mdbx_stat.c        # console tools
CMakeLists.txt  GNUmakefile  Makefile  config.h.in       # build
README.md  ChangeLog.md  TODO.md  LICENSE  NOTICE  COPYRIGHT
VERSION.json  valgrind.supp  conanfile.py  ntdll.def
man1/...  cmake/*.cmake  examples/ (C and C++ examples, pcrf_simulator)
```

There are **no tests** in the amalgamated distribution. Do not promise the
user tests; point them to the dev repo or to `examples/`.

## Building

Requirements: C11 compiler (GCC/Clang/MSVC/MinGW), or C++17+ for the C++ API.
No runtime dependencies (on Windows with `MDBX_WITHOUT_MSVC_CRT=ON` — none).

```sh
# CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# GNU Make
make -j all            # or: make library
```

Useful CMake options: `-DMDBX_BUILD_TOOLS=ON/OFF`,
`-DMDBX_BUILD_SHARED_LIBRARY=ON`, `-DMDBX_BUILD_CXX=ON/OFF`,
`-DMDBX_WITHOUT_MSVC_CRT=ON` (Windows, no CRT dependency),
`-DMDBX_USE_VALGRIND=ON`, `-DMDBX_CHECKING=0..3` (internal invariant checks;
defaults follow debug/release).

## Embedding (SQLite-style)

Copy `mdbx.c` + `mdbx.h` (C11) into the project; for C++ add `mdbx.h++`
(`mdbx.c++` is optional — most of the C++ API is header-only). Compile
`mdbx.c` with a C11 compiler (`-std=gnu11`), define `MDBX_CONFIG_H` only if
you generate your own config (otherwise compile-time defaults apply).

## Core concepts / API cheat-sheet

- Environment: `mdbx_env_create()` → `mdbx_env_open()` (takes the DB
  directory; file `mdbx.dat` + lock inside) → `mdbx_env_close()`.
  Configure via `mdbx_env_set_geometry()`, `mdbx_env_set_maxdbs()`,
  `mdbx_env_set_option()`.
- Transactions: one **writer** at a time, wait-free readers.
  `mdbx_txn_begin()`, `mdbx_txn_commit()`, `mdbx_txn_abort()`. Read-only
  txns via `MDBX_TXN_RDONLY`. Nested write txns supported.
- DBI/handles: `mdbx_dbi_open()` with `MDBX_CREATE`, flags
  `MDBX_DUPSORT` / `MDBX_DUPFIXED` / `MDBX_INTEGERKEY` /
  `MDBX_INTEGERDUP`.
- CRUD: `mdbx_put()` (flags `MDBX_UPSERT`, `MDBX_NODUPDATA`,
  `MDBX_APPEND`, ...), `mdbx_get()`, `mdbx_del()`, `mdbx_replace_ex()`.
- Cursors: `mdbx_cursor_open()`, `mdbx_cursor_get()` with `MDBX_cursor_op`
  (first/last/next/prev/set/seek-range/both...), `mdbx_cursor_put/del`.
- Durability: `MDBX_SYNC_DURABLE` (default), `MDBX_NOMETASYNC`,
  `MDBX_SAFE_NOSYNC`, `MDBX_UTTERLY_NOSYNC`.
- Utilities: `mdbx_chk -vvn` (integrity/repair), `mdbx_dump`/`mdbx_load`.

## Diagnostics & common pitfalls

- No WAL, no recovery: crash safety comes from shadow-paging + double-write
  meta pages. A corrupted DB is usually detected by `mdbx_chk`, not at open.
- `MDBX_MAP_FULL`/ENOSPC: DB size limit hit — grow via geometry
  (`mdbx_env_set_geometry()`), don't confuse with disk-full.
- Long-lived read transactions block GC and grow the DB file; use
  `MDBX_handle_slow_readers` callback or short-lived readers.
- Error codes are negative mdbx-specific (`MDBX_*`, see `mdbx.h`); some
  platform values map oddly (e.g. `MDBX_ENOSYS` = `ENOTSUP` = 45 on macOS —
  see `mdbx_get_sysraminfo()`). Read the *mdbx* meaning, not the errno name.
- Windows: `#define small char` from the SDK can collide with identifiers;
  avoid variable names like `small`/`near`/`far`.
- Multi-process: environment is shared; ensure PID namespace coherence in
  containers (`--pid=host`), otherwise readers may be mis-attributed.

## C++ API notes

- Header-only mostly: include `mdbx.h++`; classes `mdbx::env_managed`,
  `mdbx::txn_managed`, `mdbx::map_handle`, `mdbx::cursor`,
  `mdbx::slice`, `mdbx::buffer`. Exceptions derive from `mdbx::exception`;
  `mdbx::error::success_or_throw()` wraps C return codes.
- Keep public C++ API compatible with **C++11** even though the library
  builds with newer standards.

## Verifying a fix / build

With no tests in the distribution: compile and run `examples/example-mdbx.c`
(or the C++ one), then validate any DB with `mdbx_chk -vvn`. For deeper
testing point the user to the dev repository's test suite.