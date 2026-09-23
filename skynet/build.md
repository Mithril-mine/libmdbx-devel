# Build, Configuration & Testing

> Part of the [Skynet project index](README.md).
> How this repository is built (CMake, GNU Make, Conan), the key options, the CI matrix,
> the **testing infrastructure** in depth (`mdbx_test`, `tests/stochastic.sh`,
> `tests/battery-tmux.sh`, GNUmakefile test targets, CTest) and the **amalgamation process**
> (`make dist`, `dist-cutoff` markers, alloy) — both areas are central for the upcoming work
> (test rework incl. possible GoogleTest integration, refactoring). Facts verified against sources.

---

## 1. Overview

| Method | Entry point | Status |
| --- | --- | --- |
| CMake | `CMakeLists.txt` (root) | **Primary/recommended**, incl. Windows (MSVC/MinGW), macOS, Android/iOS (toolchain files), HarmonyOS |
| GNU Make | `GNUmakefile` via `Makefile` thunk | Legacy but supported; GNU Make >= 3.81 + bash |
| Conan 2 | `conanfile.py` | Packaging recipe (package name `mdbx`) |

## 2. CMake build

### 2.1 Source-layout detection

`CMakeLists.txt` distinguishes two layouts by probing for files:

- **Amalgamated** (`MDBX_AMALGAMATED_SOURCE=TRUE`): single-file distribution at root
  (`mdbx.c`, `mdbx.c++`, `mdbx.h`, `mdbx-internals.h`, `config.h.in`, tool sources...).
  **This repo is not in this layout.**
- **Development** (`FALSE`): requires `.git` + full `src/` tree (`MDBX_SOURCE_DIR=src`) +
  `tests/CMakeLists.txt`. **Current state.**

Anything else → `FATAL_ERROR "The set of libmdbx source code files is incomplete!"`.

### 2.2 Versions & reproducibility

- `include(cmake/utils.cmake)` + `semver_provide(MDBX ...)` derive version from git tags;
  `MDBX_BUILD_METADATA` appends extra metadata.
- `MDBX_BUILD_TIMESTAMP` (default `$SOURCE_DATE_EPOCH` else now). Reproducible builds:
  `cmake -DMDBX_BUILD_TIMESTAMP:STRING=unknown` or `make MDBX_BUILD_TIMESTAMP=unknown`.

### 2.3 Project/subproject modes

- Subdirectory use (`PROJECT_NAME` defined) without `MDBX_FORCE_BUILD_AS_MAIN_PROJECT` →
  SUBPROJECT mode (`MDBX_MANAGE_BUILD_FLAGS_DEFAULT=OFF`).
- Else main-project mode (`MDBX_MANAGE_BUILD_FLAGS_DEFAULT=ON`, full option control).

### 2.4 Key CMake options (verified)

| Option | Default | Meaning |
| --- | --- | --- |
| `MDBX_FORCE_BUILD_AS_MAIN_PROJECT` | OFF | Full control even as subdirectory |
| `MDBX_ENABLE_TESTS` | `${BUILD_TESTING}` | Build tests (`include(CTest)`) |
| `MDBX_BUILD_CXX` | ON if C++11+ | Build C++ API |
| `MDBX_BUILD_TOOLS` | ON (OFF on iOS) | Build `mdbx_chk/copy/drop/dump/load/stat/defrag` |
| `MDBX_BUILD_SHARED_LIBRARY` | platform | Shared vs static; `mdbx-static` naming logic (~line 1234) |
| `MDBX_MANAGE_BUILD_FLAGS` | per mode | libmdbx manages its own flags |
| `MDBX_WITHOUT_MSVC_CRT` | OFF | No MSVC CRT dependency |
| `MDBX_CHECKING` | 2 (DEBUG) / 0 else | Internal checking level (-1..3); gates `CHECKS0/1/2` |
| `MDBX_DEBUG` | 1 (DEBUG) / 0 else | Debug logging level (-1..3) |
| `MDBX_FORCE_ASSERTIONS` | — | Forces `MDBX_CHECKING=2` |
| `MDBX_AVOID_MSYNC` | ON (WIN32) / OFF | Use flush-type sync instead of msync |
| `MDBX_OUTPUT_DIR` | build dir | Binary output directory |
| `MDBX_ALLOY_BUILD` | ON for non-DEBUG | Single-object ("alloy") build of the dev tree |
| `ENABLE_ASAN` / `ENABLE_UBSAN` / `ENABLE_MEMCHECK` | — | Sanitizer switches (used by make targets and CI) |
| `MDBX_NATIVE_SEH`, `MDBX_USE_MINCORE`, `MDBX_NEED_LIBATOMIC` | platform | Platform auto-setup |

C/C++ standards: C11 default (C99 fallback for old MSVC, C23 if available); C++
23→20→17→14→11 chain. Stored in `MDBX_C_STANDARD`/`MDBX_CXX_STANDARD`.

### 2.5 Targets

Library `mdbx`/`mdbx-static`; tools `mdbx_chk/copy/drop/dump/load/stat/defrag`; examples
`mdbx_legacy_example`/`mdbx_modern_example`; test framework binary `mdbx_test`; per-test
executables (see §6).

## 3. GNU Make build (legacy)

`Makefile` forwards targets to `GNUmakefile`. `make options`/`make help` list variables/targets.
`check_buildflags_tag` forces rebuild when `MDBX_BUILD_FLAGS` change (stored in `@buildflags.tag`).

| Target | Meaning |
| --- | --- |
| `all` | libs + tools |
| `lib` / `lib-static` / `lib-shared` | library only |
| `tools` / `tools-static` | tools (`.static`, `.static-lto` variants) |
| `check` | smoke + amalgamation/install check (`DESTDIR=@check-install`, `CMAKE_OPT += -Werror=dev`; runs `smoke-assertion ninja-assertions dist install test ctest`) |
| `smoke`, `smoke-singleprocess`, `smoke-fault`, `smoke-assertion`, `smoke-memcheck` | fast scenarios (§6.4) |
| `test`, `test-assertion`, `test-singleprocess`, `test-long`, `test-ci`, `test-ci-extra` | scenarios (§6.4) |
| `test-asan`, `test-ubsan`, `test-leak`, `test-memcheck` | sanitizer/valgrind (§6.4) |
| `build-stochastic` | build `mdbx_test` + libs |
| `ctest` | `cmake-build` (ninja) + `ctest --parallel nproc --schedule-random` |
| `run-ut` | run example binaries as smoke |
| `check-posix-locking-{sysv,1988,2001,2008}` | `MDBX_BUILD_OPTIONS += -DMDBX_LOCKING=5/1988/2001/2008` + `check` |
| `bench*` | ioarena benchmarks (couple/triplet/quartet) |
| `cross-gcc`, `cross-qemu`, `gcc-analyzer`, `reformat`, `doxygen`, `release-assets` | QA/analysis/docs/release |

Variables: `CC/CXX`, `CFLAGS_EXTRA`, `MDBX_DEBUG`, `MDBX_CHECKING`, `MDBX_BUILD_OPTIONS`
(e.g. `-DMDBX_CHECKING=2 -DMDBX_DEBUG=0 -DMDBX_FORCE_ASSERTIONS=1 -DMDBX_DEBUG_SPILLING=1`),
`MDBX_BUILD_TIMESTAMP`, `MDBX_BUILD_CXX` (default YES), `MDBX_BUILD_METADATA`,
`MDBX_SMOKE_EXTRA` (extra args for `mdbx_test` in smoke targets), `DESTDIR`, `prefix`, `suffix`,
`TEST_DB`, `TEST_LOG`, `TEST_ITER`.

Auto-probing: C standard (gnu11), C++ standard chain, per-OS linker flags (`uname2ldflags`/
`uname2libs`: `-lrt -latomic` Linux, `-lntdll -lwinmm` Windows, `-lkstat -lrt` Solaris), shared
lib suffix (so/dylib/dll). `TEST_ITER` (repeat count): 7 locally, 3 under CI, 2 on macOS/Windows.
`TEST_DB`/`TEST_LOG` live in `/dev/shm` (or `/tmp`), or `pwd` under CI.

## 4. Conan 2 packaging

`conanfile.py` (`required_conan_version >= 2.7`, `name='mdbx'`, `revision_mode='scm'`,
`no_copy_source=True`, `test_type='explicit'`, `build_policy='missing'`). ~40 `mdbx.*` options
mirroring `options.h`: `locking`, `cacheline_size`, `enable_bigfoot`, `enable_dbi_lockfree`,
`enable_dbi_sparse`, `enable_pgop_stat`, `enable_profgc`, `enable_refund`, `env_checkpid`,
`force_assertions`, `mmap_*`, `trust_rtc`, `txn_checkowner`, `avoid_msync`, `build_cxx`,
`build_tools`, `disable_validation`, `without_msvc_crt`, `64bit_atomic`, `64bit_cas`,
`apple.speed_insteadof_durability`.

## 5. CI

### 5.1 SourceCraft (`.sourcecraft/ci.yaml`) — primary CI

Triggers: push on the three workflow names; daily cron `42 3 * * *`.

Global env: `MDBX_BUILD_OPTIONS=-DMDBX_CHECKING=2 -DMDBX_DEBUG=0 -DMDBX_FORCE_ASSERTIONS=1 -DMDBX_DEBUG_SPILLING=1`

| Workflow | Compiler | Config | `CI_MAKE_TARGET` | C++ API |
| --- | --- | --- | --- | --- |
| `ci-linux-debug-gcc` | gcc/g++ | `-DCMAKE_BUILD_TYPE=Devel -DMDBX_BUILD_CXX:BOOL=OFF` | `smoke` | OFF |
| `ci-linux-debug-clang` | clang/clang++ | `-DCMAKE_BUILD_TYPE=MinSizeRel -DMDBX_BUILD_CXX:BOOL=ON` | `test` | ON |
| `ci-linux-release-spilling` | clang/clang++ | `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | `check` | default |

Cube image `dh-mirror.gitverse.ru/jakoch/cpp-devbox:forky-latest`, max 30m, script
`CC=... CXX=... test*/ci/ci.sh "<cfg1>|<cfg2>"` (performs several configure+build rounds).

### 5.2 GitHub Actions (`.github/workflows/`)

`ci-linux.yml`, `ci-macos.yml`, `ci-windows-msvc.yml`, `ci-windows-mingw.yml`,
`ci-windows-mscl.yml`, `ci-cxx-msvc.yml`, `ci-android.yml` — cross-platform matrix (informational).

---

## 6. Testing — in depth

> **Verification levels**: for day-to-day work use the 7-level escalation model defined in
> [`skills/superpowers/verification-before-completion/SKILL.md`](skills/superpowers/verification-before-completion/SKILL.md):
> L1 = the specific test; L1-x = that test rebuilt under ASAN/UBSAN/MEMCHECK; L2 = full `ctest`
> (3–7 min); L3 = `make check` (5–10 min, incl. amalgamation); L4 = + `test-ubsan`/`test-asan`
> (10–30 min); L5 = + `test-memcheck` (30–90 min); L6 = `stochastic.sh` with a context-chosen
> option set and iteration count; L7 = human-controlled extended testing (`battery-tmux.sh` etc.).

The testing stack has four layers:

1. **`mdbx_test`** — the big stochastic test framework (CLI utility).
2. **`tests/stochastic.sh`** — orchestrator that sweeps parameters/modes and calls `mdbx_test`
   in a loop, verifying each DB with `mdbx_chk`.
3. **`tests/battery-tmux.sh`** — parallel runner of several `stochastic.sh` instances in tmux.
4. **Small C/C++ tests** — `tests/ut/` (areas: `api|cursor|cxx|dbi|env|gc|txn|issues`,
   layout с TASK-36 C2), `tests/exploits/` registered in CTest,
   plus a few `mdbx_test` scenarios wired into CTest.

### 6.1 `mdbx_test` — the stochastic test framework

Built from `tests/framework/*.c++` (plus `osal-unix.c++`/`osal-windows.c++`) into `mdbx_test`;
compiled with `-DMDBX_BUILD_TEST=1 -DMDBX_BUILD_CXX=1`, linked against libmdbx and `-lm`
(GNUmakefile) or via CMake target (tests/framework/CMakeLists.txt). Scenario modules:
`hill`, `deadread`, `deadwrite`, `forkread`/`forkwrite` (non-Windows), `jitter`, `try`, `copy`,
`append`, `ttl`, `nested`, plus the **`basic`** meta-case. Key generators in `keygen.c++`
(`keygen.split/width/mesh/rotate/offset/case`, cases: random/dashes/custom). `actor_testcase`
enum and status mapping in `config.h++`.

Options (verified from `main.c++`; `--option` or `--option=value`):

| Group | Options |
| --- | --- |
| Run control | `--duration NN[s|m|h]`, `--nops NN`, `--timeout NN`, `--delay NN`, `--wait4ops NN` / `--no-wait4ops`, `--repeat NN`, `--threads NN`, `--failfast`, `--dump-config`, `--loglevel LEVEL` |
| Database | `--pathname PATH`, `--pagesize N`, `--size-upper-upto N`, `--shrink-threshold N`, `--growth-step N`, `--max-readers N`, `--max-tables N`, `--drop`, `--cleanup-before`/`--cleanup-after`/`--dont-cleanup-after`, `--ignore-dbfull` |
| Table shape | `--table=+/-key.integer,+/-data.multi,+/-data.fixed,+/-data.integer`, `--keylen[.min/.max]`, `--datalen[.min/.max]` (`rnd`), `--keygen.*`, `--prng-seed N` |
| Mode flags | `--mode=+/-writemap,+/-lifo,+/-nosync-safe,+/-nosync-utterly,+/-nometasync,+/-nostickythreads,+/-perturb,+/-nomeminit,+/-nordead,+/-validation` (comma list) |
| Batches | `--batch.read N`, `--batch.write N` |
| Special | `--speculum`, `--random-writemap`, `--random-treeopts`, `--geometry-jitter`/`--no-...`, `--defrag-jitter`, `--progress`, `--console=no`, `--inject-writefault N`, `--case NAME` |
| Testcase selectors | `basic`, `--hill`, `--jitter`, `--dead.reader`, `--dead.writer`, `--try`, `--copy`, `--append`, `--ttl`, `--nested`, `--fork.reader`, `--fork.writer` |

`--speculum` enables a second, independent in-memory model to cross-check CRUD results
(used for small `--nops`/`--batch.write` runs).

### 6.2 `tests/stochastic.sh` — parameter-sweep orchestrator (798 lines)

Flow: parse options → platform prep → RAM estimation → (optionally) `make mdbx_test mdbx_chk`
→ build caseset → iterate nops/batch ladder → run probes (each probe = `mdbx_test` + `mdbx_chk`
on the DB and its online copy) → report/fail.

Options: `--multi` (default; multi-process cases) / `--single` (nested/hill/append/ttl/copy for
QEMU) / `--nested|--hill|--append|--ttl` (single case); `--with-valgrind`, `--with-gdb`,
`--skip-make`; `--from NN`, `--upto NN`, `--repeat NN`, `--rounds NN`, `--loops NN`;
`--probe-duration NN[s|m|h]`, `--whole-duration NN[s|m|h|d]`, `--delay NN`; `--dir PATH`,
`--db-upto-mb NN`, `--db-upto-gb NN`, `--no-geometry-jitter`, `--pagesize min|max|NN`,
`--numa NODE`, `--dont-check-ram-size`, `--extra`, `--taillog`, `--report-depth`, `--small`,
`--help`.

Details worth knowing:

- RAM budgeting: DB ≤ `(ram_avail − 1234) / 4` MB (comment explains dirty-page accumulation +
  online copy), capped by `--db-upto-mb`; skippable with `--dont-check-ram-size`.
  Exports `MALLOC_CHECK_=7 MALLOC_PERTURB_=42`; sets Linux `core_pattern=core.%E.%s.%p`.
- Logging: full probe log compressed via `lz4`/`gzip` (`--taillog` dumps tail on failure);
  `tee -i -p` for pipe safety; `SIGPIPE` ignored.
- Caseset generation: for every bitmask over `options=(perturb nomeminit nordead writemap lifo
  nostickythreads validation)` (or `(writemap lifo nostickythreads)` without `--extra`), the
  mode string is `bits2options` (each `+opt`/`-opt`, comma-joined) plus a cycling syncmode from
  `("", +nosync-safe, +nosync-utterly, +nometasync)`. For each combo: table configurations
  (`int-key w/o-dups`, `int-key with-dups`, `int-key int-data`, `w/o-dups`, `with-dups`,
  `int-key fixdups`, `fixdups`) across splits 30, 24, 16, (10 only with `--extra`), 4.
- Probe command: `mdbx_test [--duration=Ns] [--speculum] --random-writemap=no --ignore-dbfull
  --repeat=$REPEAT --pathname=$DIR/long.db --cleanup-after=no --geometry-jitter=yes
  --prng-seed=$seed --pagesize=$PAGESIZE --size-upper-upto=${db_size_mb}M <case-args>
  --nops=$nops --batch.write=$wbatch <case>`; then `mdbx_chk long.db` (+ `long.db-copy`).
- Iteration: nops ladder `10 33 100 333 1000 ... 1e9` (×10/3), each with
  `wbatch=nops/7+1` then repeatedly `/7` while `nops/wbatch ≤ 1000`; `--speculum` when
  `nops ≤ 1000`. `--small` iterates wbatch instead. `--loops` bounds outer loops,
  `--rounds` repeats each nops/wbatch round with a fresh `--prng-seed`.
- Under GDB: wraps `mdbx_test`/`mdbx_chk` with `gdb --init-command=tests/.gdbinit
  --command=tests/with.gdb --return-child-result`.

### 6.3 `tests/battery-tmux.sh` — parallel stochastic batteries

Base command: `stochastic.sh --skip-make --db-upto-gb 32`, workdir prefix `/dev/shm/mdbxtest-`.
Creates tmux session `mdbx` (first window `htop`), then for each `page-size ∈ {min, 4k, max}` ×
`from ∈ {1, 30000}` × `n ∈ {0..3}` spawns windows/splits running
`stochastic.sh --delay $((n*7)) --page-size $ps --from $from --dir $PREFIX...`; an `--extra`
variant uses `--delay $((3+n*7))`. If multiple NUMA nodes exist, commands cycle through
`--numa <node>` (`numactl --membind/--cpunodebind`). Attaches to the session at the end.

### 6.4 GNUmakefile test targets (non-amalgamated section)

`TEST_TARGETS = ctest mdbx_legacy_example mdbx_modern_example test-stochastic`;
`test: $(TEST_TARGETS)`; `build-test: cmake-build build-stochastic`.

| Target | What it does |
| --- | --- |
| `smoke` | 2× `mdbx_test --duration 100 --table=+data.integer --keygen.split=29 --datalen.min=min --datalen.max=max --progress --console=no --repeat=$TEST_ITER --pathname=$TEST_DB --dont-cleanup-after $MDBX_SMOKE_EXTRA basic` — first default mode, second `--mode=-writemap,-nosync-safe,-lifo`; log gzipped, `tail -n 99`; then `mdbx_chk -vvn` on DB and copy |
| `smoke-singleprocess` | `--hill` (repeat 42), `--copy` (repeat 2), `--nested` (repeat 42, `--mode=-writemap,-nosync-safe,-lifo`) + `mdbx_chk -vvn` |
| `smoke-fault` | `mdbx_test --duration 300 --inject-writefault=42 --dump-config basic` expecting a fault, then `mdbx_chk -vvnw` (repair mode) |
| `smoke-assertion`/`test-assertion` | same as smoke/test with `MDBX_CHECKING=2` |
| `smoke-ubsan`/`test-ubsan` | `CFLAGS_EXTRA += -DENABLE_UBSAN -Ofast -fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize-recover=all`, `MDBX_CHECKING=2`, `CMAKE_OPT` ENABLE_UBSAN=ON |
| `smoke-asan`/`test-asan` | `CFLAGS_EXTRA += -Os -fsanitize=address`, `MDBX_CHECKING=2`, ENABLE_ASAN=ON |
| `test-leak` | re-run `test-stochastic` with `CFLAGS_EXTRA="-fsanitize=leak"` |
| `smoke-memcheck`/`test-memcheck` | `VALGRIND=valgrind --trace-children=yes --log-file=valgrind-%p.log --leak-check=full --track-origins=yes --read-var-info=yes --error-exitcode=42 --suppressions=valgrind.supp`; `CFLAGS_EXTRA=-Ofast -DENABLE_MEMCHECK`, `MDBX_CHECKING=1`; `test-memcheck` also runs `stochastic.sh --with-valgrind --loops 2 --db-upto-mb 256 --skip-make` |
| `test-stochastic` | `tests/stochastic.sh --whole-duration 600 --probe-duration 60 --dont-check-ram-size --loops 2 --db-upto-mb 256 --skip-make --taillog` |
| `test-long` | `tests/stochastic.sh --loops 42 --db-upto-mb 1024 --extra --skip-make --taillog` (weeks) |
| `test-singleprocess` | `tests/stochastic.sh --single --loops 2 --whole-duration 600 --probe-duration 60 --dont-check-ram-size --db-upto-mb 256 --skip-make --taillog` |
| `test-ci` | loop: `check smoke-singleprocess smoke-fault smoke-memcheck test-leak test-asan test-ubsan test-singleprocess test-memcheck` |
| `test-ci-extra` | `test-ci` + `cross-gcc` + `cross-qemu` |
| `check` | `clean | smoke-assertion ninja-assertions dist install test ctest` with `DESTDIR=@check-install` |

Sanitizer env defaults: `ASAN_OPTIONS=log_path=asan.log:poison_history_size=42`,
`UBSAN_OPTIONS=log_path=ubsan.log:print_stacktrace=1`. Valgrind suppressions file: `valgrind.supp`.

### 6.5 CTest integration (`tests/CMakeLists.txt`, `tests/ut/issues/CMakeLists.txt`)

- Helper `add_simple_test(name, SOURCE, LIBRARY, TIMEOUT, DEPEND, DLLPATH, DISABLED)` →
  `add_executable(test_extra_<name>)` + `add_test(extra_<name> ...)` (skipped when
  cross-compiling without emulator). `add_extra_test` = `add_simple_test TARGET_PREFIX test_extra_`.
- Registered unit tests — `tests/ut/<area>/` (areas: `api`, `cursor`, `cxx`, `dbi`,
  `env`, `gc`, `txn`, `issues`; layout с TASK-36 C2): напр. `upsert_alldups`,
  `dupfix_addodd`, `details_rkl`, `global_init` (api), `cursor_closing`,
  `doubtless_positioning`, `distance_scroll_distribute` (cursor), `dbi_nested_txn`
  (dbi, UNIX), `rename_dbi`, `nested_drop_abort`, `early_close_dbi`,
  `maindb_ordinal`, `dupfix_multiple` (dbi), `crunched_delete`,
  `reverse_insertions` (txn), `hex_base64_base58` (api); C++ ones (when
  `MDBX_BUILD_CXX`) и long `TIMEOUT 10800`. Каждый тест несёт root-метку `ut` +
  метку области (`ut.api|cxx|env|dbi|txn|cursor|gc|issues`) — выбор по
  `tests/select-tests.sh`.
- Framework-based CTest scenarios (mdbx_test): `smoke` (`--duration=5m --loglevel=notice
  --prng-seed=$seed --progress --console=no --pathname=smoke.db --dont-cleanup-after basic`)
  followed by `smoke_chk` (`mdbx_chk -nvv`), `smoke_chk_copy`, `smoke_copy_asis`
  (`mdbx_copy -f`), `smoke_copy_compactify` (`mdbx_copy -f -c`); `dupsort_writemap`
  (`--mode=+writemap --table=+data.fixed --keygen.split=29 --datalen=rnd`) + chk tests;
  `uniq_nested` (`--mode=-writemap,-nosync-safe,-lifo`) + chk tests.

### 6.6 Per AGENTS.md rules

- Build with CMake, test with CTest.
- Build and test at least on **Linux and Windows**.

### 6.7 Test-time budget rules (owner strategy, Kaizen 2026-09-21/22)

Tests and CI dominate the schedule (>90% of wall time); every check must be as
fast and as targeted as possible.

- **LTO is forbidden in test builds unless explicitly required.** With LTO each
  test re-optimizes the whole library separately (hours). LTO applies only to
  the library/tools; test configurations must be built without LTO by default.
  Configure test build dirs with `-DINTERPROCEDURAL_OPTIMIZATION=OFF`
  (the option defaults ON for non-Debug builds); keep LTO ON only for
  release/library/tools builds that explicitly require it.
- **ccache is mandatory for test/re-iterative builds** (owner policy + TASK-23
  research, 2026-09-23): `mdbx.c` is one giant TU, so every fresh build-dir or
  worktree rebuild pays minutes of compilation per config. Wire the compiler
  launcher into every test-build configure step:
  `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`,
  with a SHARED cache: `export CCACHE_DIR=${SKYNET_ROOT}/.skynet/ccache`
  (all pool nooks share one cache; host: `apt-get install -y ccache`).
  Cache policy (owner, 2026-09-23): `max_size = 5.0G` (LRU-like eviction —
  "new displaces old" at the cap) + `max_age = 30d` in
  `~/.config/ccache/ccache.conf`; `cache_dir` points at the shared dir.
  CI runners: install ccache + `actions/cache` keyed on compiler+flags+config.
- **Adding a unit test does not justify running long stochastic `mdbx_test`
  iterations** — only build + launch sanity of `mdbx_test` (it is stochastic,
  seeded from `date+%s+RANDOM`; a fixed `--prng-seed` gives a reproducible
  per-actor sequence). Use bounded `--nops`, fixed seeds, tiered smoke
  T1/T2/T3 (`make smoke-t1/t2/t3`): T1 seconds / deterministic, T2 medium,
  T3 long — milestone/nightly only.
- **Addressability** — run only the subset intersecting the change:
  `tests/select-tests.sh` maps changed paths → CTest labels
  (see `AGENTS.md` "Segmented test execution"); do NOT blindly run everything.
- The full catalog of slow-build/test causes is tracked in BACKLOG B21–B33
  (LTO, gtest network fetch, fresh build-dirs/ccache, Debug-in-routine,
  full-length mdbx_test, mdbx_chk in every ctest, durable-sync, serial ctest,
  no impact-selection, duplicate runs, session races, full ctest on CMakeLists
  edits, test-artifact collisions).
- Validation matrices in REPORTS carry wall-clock (`build 4m12s / test 38s`)
  so duration trends stay visible.

---

## 7. Amalgamation (`make dist`) — how the "clean & flat" sources are produced

Since 2026 the project is distributed as **amalgamated single-file sources** (like SQLite):
`dist/` contains a flat set of files with no internal dependencies beyond the OS, no tests, no
internal docs. `make dist` (in this dev repo) generates `dist/` and the source tarball
`libmdbx-sources-<version>.tar.gz`. This matters for test-tooling integration (e.g. GoogleTest):
everything that must not ship in the amalgamated package is kept out via **`dist-cutoff`
markers** or by simply not being listed in `DIST_SRC`/`DIST_EXTRA`.

### 7.1 The `dist-cutoff` markers

Regions between `dist-cutoff-begin` and `dist-cutoff-end` are **deleted** when the file is
refined for `dist/`. Four comment syntaxes are recognized (see `dist-extra-rule`, GNUmakefile
lines ~930-934):

| Syntax | Used in |
| --- | --- |
| `#> dist-cutoff-begin` … `#< dist-cutoff-end` | shell/make/python style: `GNUmakefile`, `Makefile`, `CMakeLists.txt`, `cmake/*.cmake`, `conanfile.py`, `tests/ci/ci.sh`-style scripts |
| `// > dist-cutoff-begin` … `// < dist-cutoff-end` | C/C++ line comments: `mdbx.h`, `mdbx++/*.h++` (wraps `#pragma once`, `namespace mdbx {`, header guards) |
| `/* > dist-cutoff-begin */` … `/* < dist-cutoff-end */` | block comments (covered for future use) |
| `<!-- dist-cutoff-begin -->` … `<!-- dist-cutoff-end -->` | HTML/XML comments: `README.md` (dev-only banner, stochastic-test notes) |

Concrete examples found by scan: `README.md` (dev banner lines 1-8, stochastic notes),
`GNUmakefile` (help text for dev-only targets, amalgamated-vs-dev conditional branches,
`TEST_TARGETS`/`build-stochastic`, ALLOY/dist rules), `CMakeLists.txt` (the whole
development-layout `elseif` branch, tests/examples wiring, `MDBX_ALLOY_BUILD` block),
`cmake/utils.cmake` (git-version machinery), `cmake/compiler.cmake`, `conanfile.py`
(`Git` import, `fetch_versioninfo_from_git`), `mdbx.h` (`MDBX_AMALGAMATED_SOURCE` 1→0 toggle),
`mdbx++/*.h++` (every header's `#pragma once` + `namespace mdbx {`/`}`).

### 7.2 `dist` target chain

```
dist: tags @dist-checked.tag libmdbx-sources-<version>.tar.gz
@dist-checked.tag: (all dist artifacts)  → verify: copy dist/ → @dist-check → make all check ninja-assertions
%.tar.gz/.xz/.bz2/.zip/.zpaq: @dist-checked.tag → package $(DIST_SRC) $(DIST_EXTRA)
```

- `tags`: `git fetch --tags --force` (version info required).
- `@dist-checked.tag`: greps that `define xMDBX_ALLOY` did not absorb `MDBX_BUILD_SOURCERY`
  (sed correctness check), then **builds and tests a standalone copy** `@dist-check`
  (`make -C @dist-check all check ninja-assertions`) — this guarantees the amalgamated output
  is self-sufficient before packaging.
- `check` target also depends on `dist`, so a full `make check` exercises amalgamation too.
- `release-assets` additionally verifies the tree is at a clean annotated `v*` tag.

### 7.3 `DIST_SRC` / `DIST_EXTRA` — what ships

```
DIST_SRC  := mdbx.h mdbx.h++ mdbx.c mdbx.c++ mdbx_<tool>.c (chk,copy,drop,dump,load,stat,defrag)
             mdbx-internals.h mdbx-wingetopt.h
DIST_EXTRA:= LICENSE NOTICE COPYRIGHT README.md TODO.md CMakeLists.txt GNUmakefile Makefile ChangeLog.md
             VERSION.json config.h.in ntdll.def windows-safeseh-{masm,yasm}.asm windows-safeseh.obj
             valgrind.supp conanfile.py man1/* examples/{CMakeLists.txt,example-mdbx.c++,example-mdbx.c,
             pcrf/pcrf_simulator.c,README.md} cmake/{compiler,profile,utils}.cmake
```

NOT shipped: `tests/**` (framework, ut, issues, exploits, scripts), `src/**` except the tool
sources inlined into `mdbx_<tool>.c`, internal docs, `.github/`, `.sourcecraft/`, `.codeassistant/`,
`skynet/`. `mdbx.h` is generated via `dist-extra-rule` (cutoff removal) — **note**: the dev
`mdbx.h` must stay valid after cutoff deletion, and the `MDBX_AMALGAMATED_SOURCE` value flips
from 0 to 1 in the shipped copy (lines 196-201 of `mdbx.h`).

### 7.4 The alloy mechanism

- `src/alloy.c` defines `#define xMDBX_ALLOY 1` and `#include`s **all** `src/*.c` in order
  (api-*, audit, chk, …, walk, windows-import). In `essentials.h`, `xMDBX_ALLOY` turns
  `MDBX_INTERNAL` into `static` — that is how the single translation unit keeps internal
  linkage.
- Regular GNUmake/CMake dev builds compile `src/alloy.c` directly for the library objects
  (`mdbx-static.o`/`mdbx-dylib.o` depend on `src/alloy.c $(ALLOY_DEPS)`), i.e. the dev tree
  itself is built as one alloyed TU; CMake option `MDBX_ALLOY_BUILD` controls this
  (OFF for DEBUG builds → per-module objects).
- `ALLOY_DEPS = git ls-files src/ | grep -v '/tools' -v '/man'` — all engine sources.

### 7.5 Sed-based inlining pipeline (GNUmakefile rules)

| Artifact | Production rule (essence) |
| --- | --- |
| `dist/@tmp-amalgam.inc` | `src/amalgam.in` with version placeholders (`@MDBX_GIT_DESCRIBE@`, `@MDBX_GIT_TIMESTAMP@`) — the banner prepended to every amalgamated file |
| `dist/mdbx-internals.h` | `src/alloy.c` minus `#include` lines + `#define MDBX_BUILD_SOURCERY <sha256(src/version.c)>_<version>`; then `essentials.h` with `preface.h`, `osal.h`, `options.h`, `atomics-types.h`, `layout-dxb.h`, `layout-lck.h`, `logging_and_debug.h`, `utils.h`, `pnl.h` inlined at their `#include` markers (sed `r`); then strip `#include "..."`, `#pragma once`, `clang-format off/on`, `*INDENT-O*`; drop `///` doxygen lines; prepend banner |
| `dist/@tmp-squashed.inc` | `internals.h` with all its module headers inlined (`atomics-ops, proto, rkl, txl, unaligned, comparators, cogs, cursor, dbi, dml, dpl, gc, lck, meta, node, page-iov, page-ops, spill, sort, rthc, walk, windows-import`); `#include "essentials.h"` rewritten to `@INCLUDE "mdbx-internals.h"`; same stripping |
| `dist/mdbx.c` | squashed + every `src/*.c` except `alloy.c` + `src/version.c`; `debug_begin.h`/`debug_end.h` inlined at markers; includes stripped; banner |
| `dist/mdbx.h` | `dist-extra-rule` on dev `mdbx.h` (cutoff removal + version subs) |
| `dist/mdbx.h++` | dev `mdbx.h++` with all `mdbx++/{begin,decl_*,impl_*,end}.h++` inlined at include markers; `#include "../mdbx.h"` → `@INCLUDE "mdbx.h"`; explicit `/dist-cutoff-begin/,/dist-cutoff-end/d` (loose marker match, because `// >` syntax); remaining includes stripped |
| `dist/mdbx.c++` | squashed minus the `#ifndef __cplusplus` guard + `src/mdbx.c++`; `#include "../mdbx.h++"` → `@INCLUDE "mdbx.h++"` |
| `dist/mdbx_<tool>.c` | `src/tools/<tool>.c` with `essentials.h`→`mdbx-internals.h`, `wingetopt.h`→`mdbx-wingetopt.h`; `#define xMDBX_ALLOY` removed (tools keep external linkage semantics); banner |
| `dist/mdbx-wingetopt.h` | `src/tools/wingetopt.{h,c}` concatenated, includes stripped |
| `dist/VERSION.json` | generated JSON (`git_describe`, `git_timestamp`, `git_tree`, `git_commit`, `semver`) |
| `dist/config.h.in`, `dist/man1/mdbx_*.1`, `dist/.clang-format-ignore`, ntdll/safeseh assets | refined/copied as appropriate |

The `#include "..."` stripping works because every include that must survive is rewritten to
`@INCLUDE "..."` first, then restored after deletion — leaving only the intended
cross-file includes (`mdbx.h`, `mdbx.h++`, `mdbx-internals.h`).

### 7.6 Implications for adding GoogleTest / other test dependencies

- Tests already live exclusively under `tests/`, which is **not** part of `DIST_SRC`/
  `DIST_EXTRA`; a GTest-based suite added there will not leak into `dist/` by construction.
- Any new CMake hooks for tests must be placed inside `dist-cutoff` regions (or guarded by
  `NOT MDBX_AMALGAMATED_SOURCE`) so the amalgamated `CMakeLists.txt` stays free of them —
  exactly like the existing `MDBX_ENABLE_TESTS`/`add_subdirectory(tests)` wiring.
- `GNUmakefile` additions for GTest targets must also be inside `#> dist-cutoff-begin` …
  `#< dist-cutoff-end` (pattern: `TEST_TARGETS += test-stochastic`).
- New repo files that should ship in the amalgamated package must be added to `DIST_EXTRA`
  (and pass the standalone `@dist-check` build); everything else is excluded automatically.
- If GTest is fetched/bundled, prefer locating it under `tests/` (e.g. `tests/third_party/`)
  and marking it with the same cutoff conventions if any build file must reference it.

---

## 8. Reproducibility & containers

- Set `MDBX_BUILD_TIMESTAMP` (§2.2). Toolchain must also be reproducible.
- Containers: single physical copy of mapped pages; shared PID namespace (`--pid=host`);
  libmdbx/libc/pthreads versions must match host/containers (compare `mdbx_chk -V` options
  string). WSL1 unsupported (returns `ENOLCK`).

## 9. Common gotchas

- Non-GNU `make` (BSD) fails; use `gmake`.
- macOS: needs GNU sed/tar (`brew install bash make cmake ninja gnu-sed gnu-tar`).
- Windows: MSVC 2019+ recommended; MinGW 10.2+; `-DMDBX_WITHOUT_MSVC_CRT:BOOL=ON` drops CRT deps.
- `MDBX_CHECKING`/`MDBX_DEBUG` gate the internal `CHECKS0/1/2` and logging (see
  [`structure.md`](structure.md) §2.7).
- The framework tests are **stochastic**: a fix must survive `test-stochastic` (and ideally a
  `test-long`/battery run) plus the deterministic `ut/` regressions in CTest.
- `make dist` requires git tags present and a clean-enough tree for `@dist-checked.tag`;
  amending `DIST_SRC`/`DIST_EXTRA` or moving a file in/out of `dist-cutoff` regions changes
  the shipped artifact set.