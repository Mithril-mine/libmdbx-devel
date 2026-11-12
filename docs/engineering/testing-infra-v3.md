# Testing Infrastructure v3 — Design (implemented)

> Part of the [Skynet project index](README.md).
> Status: **implemented on `master`** (ci-dispatch/ci-run + `tests/ci/*`). This document
> describes the shipped design; the v2 background lives in
> [testing-infra-design.md](testing-infra-design.md), the operational rules (C0–C6,
> budgets, flaky policy) in [testing-codex.md](testing-codex.md). The runner-level
> supersede/preservation semantics are described in
> [workflows.md §4a](workflows.md).

## 1. Components

- **Registry** — `tests/ci/config.json`, single source of truth: 192 build/test cells
  (`id`, `runs-on`, `cmake` flags incl. the `flag|value` pair form, `ctest`
  regex/exclude/runtime, `env`, optional `coverage`/`build_only`/`ndk`/`emulator`/
  `expected_sec`/`build_parallel`), profiles, and `common_prefixes` (12 entries).
- **Two-tier change addressing** — `tests/ci/select-cells.py`: a diff touching any
  `common_prefixes` entry selects the change-addressed gate
  (`push-quick` ∪ `win-gate`, covering the full Windows axis matrix), otherwise
  `push-quick`. Explicit path lists, fixed profiles (`--profile`) and a diversity-
  preserving cap (`--cap N`) are supported.
- **Dispatcher** — `.github/workflows/ci-dispatch.yml`: `push` on any branch and
  `pull_request` (devel/master) run the orchestrated change-addressed gate;
  `workflow_dispatch`/`repository_dispatch` carry `profile`/`cell` + `ref` for agents
  (a bare event falls back to `push-quick`). The `validate-config` job blocks the
  whole dispatch on registry inconsistency. Nightly ownership belongs to the
  per-platform workflows, not to the dispatcher.
- **Orchestrator** — `tests/ci/orchestrate.py`, the sole owner of run cancellation
  (no native `concurrency` group by design): the diff anchor is the nearest
  successful base run (temporary `ci-ok-<run_number>` tags, searched within the
  last 30 days; shas rewritten away by history rewrites are skipped quietly);
  linear-history runs are superseded, force-pushed (history-rewritten) runs
  cancelled, same-SHA push/PR duplicates deduplicated (PR wins); a follow-up diff
  that touches no common prefix preserves the superseded run in flight and spawns
  only the complement (issue #224). Branch names are normalized before the Actions
  API call.
- **Runner** — `.github/workflows/ci-run.yml` (`workflow_call`): exactly one registry
  cell per call, scope `fast`|`fast+hygiene`|`full`, per-cell env/cmake/ctest
  semantics (build-only, emulator, NDK), `cell-metrics-<cell>` metrics artifact,
  coverage export and failure artifacts.
- **Matrix resolution** — `tests/ci/resolve-cells.py`: registry ids → matrix rows
  `{cell, runs-on, scope, coverage, ndk}`, stably ordered slowest-first
  (`expected_sec`) to cut the makespan under the runner-queue throttle.

## 2. Profiles and nightly ownership

| Profile | Cells | Consumed by |
|---|---|---|
| `push-quick` | 7 | every push / PR gate baseline |
| `win-gate` | 9 | added on common-code changes (Windows axis matrix) |
| `full` | 24 | fallback when no successful base anchor exists in ancestry |
| `coverage` | 3 | coverage-export cells |
| `linux-locking` | 8 | `MDBX_LOCKING` flavors |
| `win-arm64` | 24 | native ARM64-Windows cells |
| `android-emulator` | 1 | emulator pilot |
| `linux-nightly` | 9 | `ci-linux` schedule |
| `mac-nightly` | 3 | `ci-macos` schedule |
| `android-nightly` | 4 | `ci-android` schedule |

`ci-windows` nightly runs the `win-gate` profile; the platform workflows also
consume `repository_dispatch {dispatch-event}` (payload `ref`) — the shared agent
fan-out signal (together with `ci-dispatch` and `ci-mcp-shared-graph-memory`).

## 3. Validation gates

- `tests/ci/validate-config.py check-structure` — unique ids, required fields,
  runners from `KNOWN_RUNNERS`, profile references resolve, common prefixes exist
  on disk, `build_only` cells carry no ctest.
- `check-coverage` — the change-addressed gate covers every OS/toolchain/arch/build
  axis derived from cell ids (`parse_axes`); additions to the id scheme must be
  mirrored there.
- Both run as the blocking `validate-config` job of every dispatch and locally via
  `make test-ci-config`.

## 4. Extending

- **New cell**: unique id, required fields, known runner; add to profiles
  deliberately — `check-coverage` fails on gate-axis gaps. New id schemes extend
  `parse_axes`.
- **Orchestrator semantics** changes land together with tests
  (`tests/ci/test_orchestrate.py` — fake Actions-API harness over real git
  objects) and with an update of this document / workflows.md §4a.
- **No ChangeLog entries**: the CI infrastructure does not reach the amalgamated
  build (changelog-codex).
