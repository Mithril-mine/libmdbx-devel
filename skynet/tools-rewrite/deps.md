# Tools → libmdbx dependency map

Extracted from `src/tools/*.c` (exact call sites noted per tool).

## Per-tool mdbx_* API usage

| Tool | mdbx_* APIs (grep-verified) |
|---|---|
| **chk** | `mdbx_env_chk` (main), `mdbx_env_chk_encount_problem`, `mdbx_env_create`, `mdbx_env_open`, `mdbx_env_open_for_recovery`, `mdbx_env_set_maxdbs`, `mdbx_env_sync_ex`, `mdbx_env_turn_for_recovery`, `mdbx_env_warmup`, `mdbx_setup_debug`, `mdbx_strerror` |
| **copy** | `mdbx_env_copy`, `mdbx_env_copy2fd`, `mdbx_env_create`, `mdbx_env_open`, `mdbx_env_warmup` |
| **defrag** | `mdbx_env_defrag` (main), `mdbx_env_info_ex`, `mdbx_gc_info`, `mdbx_ratio2digits`, `mdbx_txn_begin/commit/abort`, `mdbx_env_warmup` |
| **drop** | `mdbx_dbi_open`, `mdbx_drop`, `mdbx_txn_begin/commit/abort`, `mdbx_env_set_maxdbs` |
| **dump** | `mdbx_dbi_open(_ex)`, `mdbx_dbi_flags`, `mdbx_dbi_stat`, `mdbx_dbi_sequence`, `mdbx_dbi_close`, `mdbx_cursor_open/get/close/ignord`, `mdbx_canary_get`, `mdbx_env_info_ex`, `mdbx_txn_begin/abort/reset/renew`, `mdbx_txn_env`, `mdbx_txn_id`, `mdbx_env_warmup` |
| **load** | `mdbx_dbi_open_ex`, `mdbx_dbi_sequence`, `mdbx_dbi_close`, `mdbx_cursor_open/bind/put/close`, `mdbx_drop`, `mdbx_canary_put`, `mdbx_env_set_geometry`, `mdbx_env_set_maxdbs`, `mdbx_env_set_maxreaders`, `mdbx_env_set_option`, `mdbx_env_get_maxkeysize_ex`, `mdbx_limits_dbsize_max`, `mdbx_txn_begin/commit/abort/checkpoint/info` |
| **stat** | `mdbx_dbi_open`, `mdbx_dbi_stat`, `mdbx_dbi_close`, `mdbx_enumerate_tables`, `mdbx_env_info_ex`, `mdbx_gc_info`, `mdbx_reader_list`, `mdbx_reader_check`, `mdbx_ratio2digits`, `mdbx_ratio2percents` |

All tools: `mdbx_version`, `mdbx_build`, `mdbx_sourcery_anchor` (version banner),
`mdbx_setup_debug` (logger callback), `mdbx_strerror`.

## Shared code

- **`src/essentials.h`** (not src/tools/): included by every tool; brings in
  libmdbx internals (`LIBMDBX_INTERNALS`) — the tools compile against internal
  headers, not just public `mdbx.h`. This is a key rewrite decision: the C++
  tools should use ONLY the public API (`mdbx.h` via `extern "C"`, or `mdbx.h++`).
- **`src/tools/wingetopt.{c,h}`** — getopt shim for Windows (see below); included
  by all getopt-based tools.
- Each tool defines its own small `logger()` + `error()` + `usage()` (duplicated
  ~6×); a shared `common.hpp` is the obvious dedup target.

## wingetopt

- Provides `getopt()`/`getopt_long()` for Windows/MSVC where POSIX getopt is
  absent. Declared in `wingetopt.h`; implemented in `wingetopt.c` (87 lines).
- GNUmakefile amalgamates it into `mdbx-wingetopt.h` for dist builds
  (`GNUmakefile:921-940`); the `dist-tool-rule` rewrites
  `#include "wingetopt.h"` → `@INCLUDE "mdbx-wingetopt.h"`
  (`GNUmakefile:931-940`).

## Build wiring (GNUmakefile)

- `TOOLS := chk copy defrag drop dump load stat` (`GNUmakefile:150`);
  `MDBX_TOOLS := $(addprefix mdbx_,$(TOOLS))`.
- Rule `mdbx_%: mdbx_%.c mdbx-static.o mdbx-wingetopt.h` compiles each tool as a
  standalone C binary linked with the static lib (`GNUmakefile:510-511`).
- CMake side: `tests/CMakeLists.txt` and root `CMakeLists.txt` also build the
  tools (check `mdbx_tools` targets during Phase B).
- Man pages built/installed from `src/man1/`; `MANPAGES` = stat, copy, dump,
  load, chk, drop (6 — **no mdbx_defrag.1**).

## Consumers of the tools

- `tests/dump-load.sh`: round-trips `mdbx_chk` → `mdbx_dump -a` →
  `mdbx_load -[a]nf` → `mdbx_chk [-i]`, and rescue variants
  (`mdbx_dump -ar` / `mdbx_load -ranf`) (`tests/dump-load.sh:15-33`).
- `tests/stochastic.sh`: references mdbx_chk/mdbx_copy in scenario tails
  (verify exact invocations during Phase B).
- CI workflows use `mdbx_chk` (`*_chk` ctest tests), `mdbx_copy`,
  `mdbx_dump`/`mdbx_load` via smoke chains and `mdbx_stat` in some probes.

## Rewrite seams

- `MDBX_chk_context_t` callback table (chk), `MDBX_defrag_func` (defrag) —
  natural C++ adapter points.
- Logger/error/usage duplication → single `common.hpp`.
- Public-API-only dependency is the main incentive to drop `essentials.h`.