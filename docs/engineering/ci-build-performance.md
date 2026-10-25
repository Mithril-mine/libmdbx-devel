# CI build performance: ccache vs PCH vs Ninja on the libmdbx Windows cells

Measured 2026-10 on GitHub Actions (`windows-latest`, x64, ~8-core runner;
arm64 runner is noticeably weaker). Repo cell registry: `tests/ci/config.json`;
single-cell driver: `tests/ci/run-cell.py` (reports `configure_ms` / `build_ms`
/ `test_ms` in `cell-metrics.json`). Cell under test unless stated:
`win-msvc-v145-x64-debug-shared-on-mscrt-off-avoid-msync-off` (Debug, VS generator).

## Results

| Build mode | configure | build | test | total |
|---|---|---|---|---|
| VS generator + PCH + ccache (status quo) | 32s | **57s** | 7s | 96s |
| VS generator + no-PCH + ccache | 35s | **81s** | 7s | 123s |
| Ninja + cl (attempt) | **fails** — `-A`/`-T` conflict with `-G Ninja`; `CMAKE_C_COMPILER not set` | — | — | — |
| MinGW Makefiles (master, mingw cell) | — | — | — | ~248s |
| Ninja (mingw cell, experiment) | 34s | **273s** | 31s | 337s |

Observations:
- **PCH pays off on MSVC**: -30 % build time (57s vs 81s). Keep the shared test
  PCH (`MDBX_TEST_PCH` default ON).
- **ccache is useless on the Visual Studio generator**: the compiler launcher
  is not invoked for MSBuild, so `ccache` cache stays empty (`Cache size 0.0/0.3 GB`
  in the cell log) while still paying cache upload/download overhead. ccache is
  only useful where the generator actually calls the launcher: Ninja / MinGW
  Makefiles / Unix Makefiles over GCC/Clang (Linux, macOS, mingw, clang-cl cells).
- **Ninja on MSVC cells** is not a drop-in: cell cmake uses `-A`/`-T`, which
  Ninja rejects, and a working setup needs `cl` + vcvars in the environment.
  The gain over MSBuild is also not guaranteed (configure cost is similar).
- **Ninja on mingw** is not faster than MinGW Makefiles (and risks OOM without
  the `build_parallel` ceiling) — keep the Makefiles generator.

## Applied policy

- `ci-run.yml`: skip the `CMAKE_C(XX)_COMPILER_LAUNCHER=ccache` and the
  `actions/cache` step for the VS-generator cells (`win-msvc-*`, `win-cxx-*`);
  GCC/Clang cells keep ccache.
- `CCACHE_MAXSIZE` raised 300M -> 1G for the platforms where ccache works.
- `MDBX_TEST_PCH` left ON by default (measured win).
- The rolling ccache key (per-cell, `github.run_number` + prefix `restore-keys`)
  stays; it makes warm-GCC/Clang rebuilds fast across runs.

## References

- ccache manual: MSVC/clang-cl compiler types; PCH requires `sloppiness`
  `pch_defines,time_macros` and `-fpch-preprocess`/`-fno-pch-timestamp`;
  "direct mode" and `hash_dir`/`base_dir` matter for cross-run hits.
- CMake: compiler launcher is honored by Ninja/Unix generators, not by the
  Visual Studio generator.
- Findings are per-cell numbers; re-measure with `cell-metrics.json` if the
  runner images change.
