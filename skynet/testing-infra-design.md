# Testing Infrastructure v2 — Design

> Part of the [Skynet project index](README.md).
> Status: **approved design** (owner 2026-09-21). Next step: implementation plan
> (`writing-plans`), then a task list. The old infra (7 GitHub workflows + `tests/ci/ci.sh`)
> stays untouched as a fallback until the new one is proven.

---

## 1. Goal

Build a **new test infrastructure in parallel** with the current ad-hoc one, that gives us:

- **Addressable runs**: run exactly one build configuration ("cell") on demand, or a named
  group of cells ("profile"), against any ref (branch or SHA) — from both the GitHub UI and
  programmatically (agents via `repository_dispatch`).
- **Push-minimum**: every push/merge to `devel`/`master` runs only a small representative
  set (~5 cells), not the full ~130–160-cell matrix.
- **Nightly full**: the complete matrix runs on schedule against `master` HEAD.
- **Honest platform coverage**: Android cells are explicitly build-only (no emulator yet);
  cells that are known-slow/flaky (e.g. macOS `smoke_fault`) are declared per-cell so the
  nightly full matrix doesn't burn minutes on them.

The old infra remains: `tests/ci/ci.sh` = fallback script; the 7 existing workflows lose their
`on: push` trigger and become dispatch-only fallbacks.

## 2. Non-goals

- No Android emulator/test-runner integration yet (documented as build-only).
- No SourceCraft `.sourcecraft/ci.yaml` changes for now (quota exhausted; config lives on
  `master` per platform rule; revisit when quota returns).
- No changes to the local segmented test execution model (`ut.*` labels, P0–P5, tiered smoke
  T1/T2/T3) — the new infra consumes those same CTest labels.
- No rewrite of `tests/ci/ci.sh` itself.

## 3. Key platform constraints (verified)

| Constraint | Implication |
| --- | --- |
| SourceCraft reads workflow config from **`master`** regardless of branch | SourceCraft CI can only see config merged to `master` |
| GitHub `repository_dispatch`/`schedule` fire **only if the workflow file exists on the default branch**, and run against the default-branch HEAD | The dispatcher workflow must live in `master`; requested ref must be passed explicitly and checked out inside the runner |
| GitHub `workflow_dispatch` can pick any ref after first run | Owner UI path works against any branch/SHA |
| GitHub `push` uses the workflow file from the pushed branch | Devel-side changes to the runner are testable before merging to master |
| Matrix × reusable `workflow_call` is supported | `matrix.include` can fan out to a reusable `ci-run.yml` |

## 4. Architecture

```
request (push / workflow_dispatch / repository_dispatch / schedule)
        │
        ▼
┌─────────────────────────── ci-dispatch.yml (orchestrator, lives in master) ──────────────┐
│ job resolve: request → cell list (JSON) via tests/ci/config.json registry                │
│ job run:     strategy.matrix.include = fromJson(resolve.outputs.cells)                   │
│              each cell → uses: ./.github/workflows/ci-run.yml (workflow_call)            │
└──────────────────────────────────────────┬───────────────────────────────────────────────┘
                                           ▼
                        ┌─── ci-run.yml (reusable, one cell) ───┐
                        │ checkout requested ref                 │
                        │ cmake configure+build (clean CMake)    │
                        │ ctest  (labels/exclusions per cell)    │
                        │ artifact upload on failure             │
                        └────────────────────────────────────────┘
```

### 4.1 Registry — `tests/ci/config.json`

Single source of truth. Structure:

```jsonc
{
  "cells": [
    {
      "id": "linux-gcc-devel-cxx-off",
      "runs-on": "ubuntu-24.04",
      "env": { "CC": "gcc", "CXX": "g++" },
      "cmake": ["-DCMAKE_BUILD_TYPE=Devel", "-DMDBX_BUILD_CXX:BOOL=OFF"],
      "ctest": null,                    // default label set; or explicit
      "build_only": false,              // true for Android (no emulator yet)
      "note": "SourceCraft ci-linux-debug-gcc equivalent"
    }
  ],
  "profiles": {
    "push-quick":  ["linux-gcc-devel-cxx-off", "linux-clang-minsizerel-cxx-on",
                    "win-msvc-x64-debug", "macos-15-release", "win-mingw-x64-release"],
    "win-full":    [...],
    "mac-full":    [...],
    "android-build": [...],
    "linux-full":  [...],
    "full":        [/* union of all cells except documented flaky/slow */]
  }
}
```

Cell ids follow `{platform}-{toolchain}-{target}-{build-type}[-{opts}]`, e.g.
`win-msvc-v145-arm64-debug-dll-on-crt-off`. Each cell declares exactly what today is hard-coded
in a workflow matrix row: `runs-on`, toolchain env, cmake args, ctest args.

### 4.2 Orchestrator — `ci-dispatch.yml`

- Triggers:
  - `push` → profile `push-quick` (branches: `devel`, `master`).
  - `workflow_dispatch` → inputs: `profile` (choice) or `cell` (choice/free), plus `ref`.
  - `repository_dispatch` (`types: [dispatch-event]`) → `client_payload`:
    `{"profile"|"cell": "...", "ref": "branch-or-sha"}`. For agents, `ref` is mandatory.
  - `schedule` → profile `full` on `master` HEAD (00:30 UTC daily).
- `job resolve`: reads the registry **from the requested ref** (checkout the ref, parse
  `tests/ci/config.json`), resolves profile/cell → emits JSON array of cell objects.
- `job run`: matrix over that JSON; each matrix row calls `ci-run.yml` with the cell fields.
  Report both cell-level and workflow-level conclusions.

### 4.3 Runner — `ci-run.yml` (reusable, `workflow_call`)

- Inputs: cell id, runs-on, env map (toolchain), cmake args, ctest args, ref, build_only flag.
- Steps:
  1. `actions/checkout@v7` with `ref` + `fetch-depth: 0` + `fetch-tags: true`.
  2. Set up toolchain from cell env.
  3. Pure CMake configure + build (no `ci.sh`).
  4. CTest with per-cell label regex / excludes / parallel / timeout.
  5. On failure: upload `LastTest.log` + fault artifacts.
- Cell-level `build_only: true` (Android) skips step 4.

## 5. Request → cell mapping rules

| Request | Resolved to |
| --- | --- |
| `push` on devel/master | `push-quick` profile (~5 cells) |
| `schedule` daily | `full` profile on `master` HEAD |
| `workflow_dispatch` with `cell=win-msvc-v145-arm64-...` | that single cell |
| `workflow_dispatch` with `profile=mac-full` | all mac cells |
| `repository_dispatch` `{profile, ref}` / `{cell, ref}` | resolved against `ref`'s registry |

Unknown ids / bad refs → fail-fast in `resolve` with a clear message (no silent empty matrix).

## 6. Known-flaky/slow cells policy

- Registry keeps an explicit `flaky`/`slow` flag (e.g. macOS `smoke_fault`, ARM64 long
  stochastic smoke). These are **excluded from `full` by default** but still addressable by id.
- This replaces today's inline `TEST_ARGS`/`-E smoke_sp_` hacks scattered across workflows.

## 7. Rollout phases

1. Write registry `tests/ci/config.json` mirroring current matrix inventory.
2. Add `ci-run.yml` (reusable runner), self-contained, local-buildable.
3. Add `ci-dispatch.yml` orchestrator (push-quick + workflow_dispatch + repository_dispatch).
4. Validate: targeted single-cell run against a real ref; then push-quick on a devel branch.
5. Merge infrastructure to `master` so schedule/dispatch fire there.
6. Disable `on: push` on the 7 legacy workflows (keep them dispatch-only fallback).
7. Nightly `full` on master; review first night's results, tune flaky exclusions.

## 8. Testing strategy for the infra itself

- Local: run `ci-run.yml` logic as a shell script locally (Linux cells) — the runner must be
  expressible as `tests/ci/run-cell.sh <cell-id>` reusing the registry, so agents can
  reproduce a failing cell locally without GitHub.
- CI: a single-cell dispatch run is the acceptance test for the dispatcher; push-quick on a
  throwaway branch validates the push path without touching devel.

## 9. Affected files

- New: `tests/ci/config.json`, `.github/workflows/ci-run.yml`,
  `.github/workflows/ci-dispatch.yml`, `tests/ci/run-cell.sh` (local runner).
- Touched (later phase): legacy workflow files — remove `on: push` block only.
- Docs: this design + `skynet/workflows.md` + `skynet/sourcecraft/README.md` note about
  master-config rule.

## 10. Open items (tracked in BACKLOG)

- Android emulator/test-runner integration (future, separate effort).
- SourceCraft `.sourcecraft/ci.yaml` migration once quota returns.
- Whether `full` should also cover `stable`/`lts` branches nightly.