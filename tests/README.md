# libmdbx test suite

This directory contains the test suite and related internal development
components of [_libmdbx_](../README.md). These components are **not** part of
the _libmdbx_ library itself and are **not** included into the amalgamated
distribution produced by `make dist`.

## Licensing

The content of this directory is distributed under a **non-free proprietary
license**, which differs from the Apache License, Version 2.0 that governs the
_libmdbx_ library itself. Please refer to the [LICENCE](LICENCE) file in this
directory for the full license text, and to the root [README](../README.md) for
the library license information.

## Structure

| Path | Purpose |
| ---- | ------- |
| [`ut/api`](ut/api), [`ut/cxx`](ut/cxx), [`ut/env`](ut/env), [`ut/dbi`](ut/dbi), [`ut/txn`](ut/txn), [`ut/cursor`](ut/cursor), [`ut/gc`](ut/gc), [`ut/issues`](ut/issues) | Unit tests written on top of [googletest](https://github.com/google/googletest), one test-case per file, placed into a subdirectory matching the primary area. Tests are tagged with a hierarchical label tree (`ut.api`, `ut.cxx`, `ut.env`, `ut.dbi`, `ut.txn`, `ut.cursor`, `ut.gc`, `ut.issues`), plus the orthogonal `ut.heavy` attribute for slow stress tests. |
| [`framework/`](framework) | The stochastic/actor-based test framework: testcase actors (`hill`, `try`, `append`, `jitter`, `ttl`, `nested`, `fork`, `dead`, `copy`), key/value generators, OS abstraction and the `mdbx_test` entry point. Used by the smoke scenarios and the long stochastic runs. |
| [`ci/`](ci) | CI helper scripts, primarily `ci.sh` which performs the CMake/Make build-and-test pipeline used by CI workflows. |
| [`exploits/`](exploits) | Proof-of-concept programs for historically reported vulnerabilities, kept for reference. |
| `stochastic.sh` | Driver for the long stochastic test scenario. |
| `dump-load.sh` | Helper for dump/load round-trip testing. |
| `battery-tmux.sh`, `tmux.conf` | Helpers for running test batteries inside `tmux`. |

## Building and running

The tests are registered and built via CMake/CTest. With an existing build
directory (e.g. `@cmake-build`), the unit tests can be run selectively by their
labels:

```sh
# fast unit tests only (default for routine changes)
ctest -L 'ut\.' -LE 'ut\.heavy'

# a specific area (e.g. the C++ API)
ctest -L 'ut\.cxx'

# a single test
ctest -R '^txn-dataops$'
```

Smoke scenarios are also wired into `make smoke` (see the root
[GNUmakefile](../GNUmakefile)) and the stochastic scenario is available via
[`stochastic.sh`](stochastic.sh).

## Tiered smoke profiles (T1/T2/T3)

`mdbx_test` is stochastic, so the tiered profiles use **fixed `--prng-seed`**
and bounded `--nops` to keep runs reproducible and fast. Address them via CTest
labels or GNUmakefile targets:

```sh
ctest -L 'smoke-t1'    # or: make smoke-t1   T1 fast deterministic smoke (seconds)
ctest -L 'smoke-t2'    # or: make smoke-t2   T2 medium smoke (quick-smoke family)
ctest -L 'smoke-t3'    # or: make smoke-t3   T3 long/full stochastic (milestone/nightly)
```

- **T1** never runs long iterations: bounded `--nops`, `--repeat=1`, and
  `+nosync-safe` mode when durability is not the point.
- **T3** needs `cmake-stochastic-build` / `MDBX_ENABLE_LONG_TESTS=ON`.
- **Single-process / cross-qemu** (`smoke_sp_*`, label `smoke-singleprocess`)
  is registered only with `-DMDBX_ENABLE_SINGLEPROCESS_TESTS=ON`; it is meant
  for cross-qemu/cross-toolchain runs (`make smoke-singleprocess` or the
  `cross-qemu` target configure it automatically) and is absent from a plain
  build dir (`ctest -L smoke-singleprocess` yields 0 tests there). The fast
  deterministic twins `smoke_t1_hill/nested/copy` cover the same scenarios in
  the default corpus.
- A plain `ctest` in a default build dir runs `ut.*` plus the T2 quick smoke
  family and the T1 tier (no 5-minute T3 runs, no single-process chain).
- See [`mdbx-test-options.md`](mdbx-test-options.md) for the full option
  reference, and `select-tests.sh` for impact-based selection.

Notes for CI consumers:

- The three T1 tests also carry the plain `smoke` label, so any `ctest -L smoke`
  run includes them (~seconds each). This is intended: T1 is the fastest tier
  and doubles as a quick sanity check.
- Wall-clock budgets on a typical dev host: T1 ~ seconds; T2 (quick-smoke
  family incl. `smoke_fault` up to 1800 s timeout and the `smoke_sp_*` chain)
  ~ a few minutes on fast hosts but can grow on slow ones; T3 (stochastic
  suites) ~ tens of minutes to hours. Choose the tier that matches the change
  and the host load (`nice 10`), not a full corpus.

See the "Testing instructions" in the root [AGENTS.md](../AGENTS.md) for the
full segmented-test-execution policy.