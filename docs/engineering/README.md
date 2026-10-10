# Skynet — Project Index for libmdbx (devel)

> **RU:** Это индекс и отправная точка по репозиторию `libmdbx-devel` (org `dqdkfa`, платформа SourceCraft).
> Здесь собрано: карта репозитория, системы сборки, внутренняя архитектура, рабочие процессы,
> интегрированные агентные практики (Superpowers) и специфика платформы SourceCraft.
> Общие материалы лежат в этом каталоге, специфика SourceCraft — в [`sourcecraft/`](sourcecraft/README.md).

This directory is the **starting point and index** for agents working in the
[`libmdbx-devel`](https://sourcecraft.dev/dqdkfa/libmdbx) repository.
It was produced by scanning and classifying the repository contents, and it is meant to be
maintained as the codebase evolves.

- General-purpose documentation (useful for any agent or contributor): `skynet/*`
- SourceCraft-platform specifics (CI config, MCP, PR flow): `skynet/sourcecraft/*`
- Integrated agent methodology (adapted from [`obra/superpowers`](https://github.com/obra/superpowers), MIT): `skynet/skills/superpowers/*`

---

## 1. What this project is

**libmdbx** (aka MDBX) is an extremely fast, compact, powerful, embedded, transactional
key-value database, Apache-2.0 licensed, written in C11 with an optional C++ API layer.
It is a deeply revised and extended descendant of LMDB, focused on creating unique lightweight
solutions: ACID, MVCC + copy-on-write, memory-mapped, B+tree, no WAL and no crash recovery,
wait-free parallel readers, single writer serialization.

This repository contains the **non-amalgamated development source code** (permanently in
development, not for embedding). The amalgamated, easy-to-embed source is distributed separately
via [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx) / GitHub mirror
([Mithril-mine/libmdbx](https://github.com/Mithril-mine/libmdbx)).

Author: Leonid Yuriev \<leo@yuriev.ru\>. Home site: <https://libmdbx.dqdkfa.ru>.

## 2. Quick facts

| Aspect | Value |
| --- | --- |
| License | Apache-2.0 (since v0.13, May 2024; see `COPYRIGHT` for history) |
| Languages | C11 (`-std=gnu11`), C++17..23 for the C++ API, bash for tooling |
| Primary build | CMake (>= 3.12); legacy GNU Make (`GNUmakefile`, via `Makefile` thunk) |
| Packaging | Conan 2 recipe (`conanfile.py`) |
| Tests | CTest-based; unit tests in `tests/ut/`, regression in `tests/ut/issues/`, framework in `tests/framework/` |
| CI | SourceCraft `.sourcecraft/ci.yaml` (Linux gcc/clang) + GitHub Actions `.github/workflows/` (cross-platform) |
| Code style | LLVM (`clang-format`, `.clang-format` at repo root) |
| Platforms | Linux, Windows (MSVC/MinGW), macOS, Android, iOS, FreeBSD, DragonFly, Solaris, NetBSD, OpenBSD, HarmonyOS, POSIX.1-2008 |
| Origin | forked from LMDB in 2015; since 2022 canonical origin on SourceCraft |

## 3. Repository map (top level)

| Path | Purpose |
| --- | --- |
| [`src/`](../../src) | Core engine, split into ~60 focused modules (public API, transactions, B-tree, GC, OS abstraction, tools, man-pages) |
| [`mdbx.h`](../../mdbx.h) | Public C API (single header) |
| [`mdbx.h++`](../../mdbx.h++) | Public C++ API (single header) |
| [`mdbx++/`](../../mdbx++) | C++ API implementation/declaration headers included by `mdbx.h++` |
| [`src/mdbx.c++`](../../src/mdbx.c++) | Non-inline part of the C++ API |
| [`tests/`](../../tests) | Test framework (`framework/`), unit tests (`ut/`), issue regressions (`issues/`), exploits, stochastic/battery scripts |
| [`examples/`](../../examples) | C and C++ usage examples, PCRF simulator |
| [`docs/`](../../docs) | Doxygen configuration and custom HTML/CSS for generated docs |
| [`cmake/`](../../cmake) | CMake helper modules (`compiler.cmake`, `profile.cmake`, `utils.cmake`) |
| [`CMakeLists.txt`](../../CMakeLists.txt) | Main CMake build (1619 lines); detects amalgamated vs non-amalgamated layout |
| [`GNUmakefile`](../../GNUmakefile) | Legacy GNU Make build (1105 lines) with many targets (`check`, `smoke`, `dist`, `doxygen`, ...) |
| [`Makefile`](../../Makefile) | Thin thunk that forwards to `GNUmakefile` |
| [`conanfile.py`](../../conanfile.py) | Conan 2 recipe with dozens of `mdbx.*` options |
| `.sourcecraft/` | SourceCraft CI config (`ci.yaml`) |
| `.github/workflows/` | GitHub Actions CI (android, linux, macos, windows-msvc/mingw/mscl, cxx-msvc) |
| [`AGENTS.md`](../../AGENTS.md) | Agent rules: CMake+CTest for build/test, Linux+Windows required, LLVM style |
| `ChangeLog*.md` | Versioned changelogs (0.09 .. 0.13, Old) |
| `COPYRIGHT`, `LICENSE`, `NOTICE` | Legal files |

## 4. Navigation

| Document | Contents |
| --- | --- |
| [`structure.md`](structure.md) | Full file-by-file map of the repository, classification of `src/` modules into subsystems |
| [`../ru/deep-dive/internals.md`](../ru/deep-dive/internals.md) | **How libmdbx works** (RU, published): functional architecture — subsystems, mechanisms, API entities and their interplay, top-down, without file/module binding; includes a reader Q&A section (big values vs GC, long readers vs DB growth, lazy-search cache, WAF) and a deep-dive part (LIFO, BigFoot, dupsort, DBI handles, bunch delete, range estimation, cursor tracking, nested txns, parking, on-the-fly resize, auto-compaction, defrag) |
| [`../en/deep-dive/internals.md`](../en/deep-dive/internals.md) | The same document in English (EN translation of the RU original) |
| [`../ru/deep-dive/improvements.md`](../ru/deep-dive/improvements.md) | Catalog of libmdbx improvements over LMDB (RU): features, API extensions, engine mechanisms, bug fixes and optimizations down to micro-details (SIMD, branchless search, atomics), with version timeline — compiled from README history and ChangeLog 0.09–0.14 |
| [`../en/deep-dive/improvements.md`](../en/deep-dive/improvements.md) | The same catalog in English (fresh EN text based on the RU original, 16 sections) |
| [`test-scenarios.md`](test-scenarios.md) | Test scenario specifications (RU) with SystemTap/DTrace probe injection points: what and how to test per subsystem, expected outcomes, and "pain points" for embedding USDT probes — a spec for the test-writing agent |
| [`module-interfaces.md`](module-interfaces.md) | Cross-module interface map: exports per module (proto.h), include backbone, verified call sites, dependency cycles |
| [`cxx-api.md`](cxx-api.md) | C++ API layer in depth: header topology, class inventory, decl/impl split, C↔C++ bridge, build/amalgamation, compiler matrix |
| [`build.md`](build.md) | Build systems (CMake, GNU Make, Conan), key options, CI matrix, testing commands |
| [`../ru/deep-dive/architecture.md`](../ru/deep-dive/architecture.md) | Internal architecture (RU, published): layers, module relationships, data flow, amalgamation; compact cross-mechanism overview for external experts: MVCC, durability/flush path, GC, file growth/geometry, mmap/memory, single-writer |
| [`debugging-methodology.md`](debugging-methodology.md) | Debugging methodology: tools (`mdbx_chk`, PROFGC, `commit_latency`, sanitizers), step-by-step scenarios (growth, slow commit, MAP_FULL, corruption, long readers, fork, Windows, containers), internal debugging |
| [`../ru/deep-dive/internals.md`](../ru/deep-dive/internals.md) | Deep dive on LCK layout/versions, RLT slots, HSR protocol, fork(), WRITEMAP/auto-sync and recovery modes |
| [`glossary-compact.md`](glossary-compact.md) | Compact glossary of terms used in discussions/reformulations (writemap, WAF, overflow, subpage, stragglers, presync, etc.) |
| [`libmdbx-invariants.md`](libmdbx-invariants.md) | **Invariants checklist (RU)**: subsystems S1–S14, 6 critical invariants, C/C++ ABI boundary, module-risk matrix — used in every review/test |
| [`subsystem-map.md`](subsystem-map.md) | Subsystem map S1–S14 with key `src/*` modules and refactoring risk levels (🔴🟠🟡) |
| [`module-lock-matrix.md`](module-lock-matrix.md) | Pairwise subsystem conflict matrix (HIGH/MED/LOW) + high-risk `src/*` zones for parallel work planning |
| [`test-durability-strategy.md`](test-durability-strategy.md) | Test durability strategy: `MDBX_SYNC_*` modes, speed-up transports (`/dev/shm`, RAM-disk), Quick/Full matrix |
| [`test-coverage.md`](test-coverage.md) | Tests → modules coverage map: which test guards which module/feature, coverage gaps |
| [`testing-methodology.md`](testing-methodology.md) | Managed/controlled testing methodology: three instrumentation channels (COLLECT/WATCH/FAULT), coverage axes, tool taxonomy, Kaizen iterations, concrete macro implementation (probe-bus v2) |
| [`testing-codex.md`](testing-codex.md) | **Operational codex C0–C6**: double-loop/Kaizen, test addressing (diff→labels/cells), tiering & budgets (P0–heavy), flake management, metrics/SLA, local/CI parity, corpus hygiene, temp-cleanup |
| [`test-scenarios-codex.md`](test-scenarios-codex.md) | Quality standard for `test-scenarios.md` (SC-*): mandatory fields, review checklist, verdicts, process |
| [`probe-bus.md`](probe-bus.md) | Probe-bus (mprobe v2): in-process registry, `MPROBE_COLLECT/WATCH/FAULT` macros, `mprobe_ctl()`, file-IPC transport, fault injection, self-tests |
| [`probes.md`](probes.md) | USDT/DTrace probe catalog: naming convention, how to enable, per-marker table (module/args/meaning), fault injection, platform validation status |
| [`systemtap-reference.md`](systemtap-reference.md) | SystemTap reference: user-space/kernel domains, session architecture, statement-probe syntax, target variables, observation vs injection, examples, error checklist |
| [`coverage-pilot-inventory.md`](coverage-pilot-inventory.md) | Coverage-pilot (B68-P3) inventory: per-test → module/function mapping, reachable-target accounting, success metric |
| [`testing-infra-design.md`](testing-infra-design.md) | Testing Infrastructure v2 design: registry (`tests/ci/config.json`), orchestrator/runner workflows, request→cell mapping, flaky policy, rollout |
| [`techdebt.md`](techdebt.md) | Tech-debt inventory from source scan: 86 TODO/FIXME/workaround markers by category with file:line refs |
| [`workflows.md`](workflows.md) | Git branching, PR/review flow, CI triggers, release & amalgamation process |
| [`skills/README.md`](skills/README.md) | SKILLS-иерархия L0–L4: superpowers/shared/roles/orchestrator, references (subsystem-map, module-lock-matrix) |
| [`skills/superpowers/README.md`](skills/superpowers/README.md) | Integrated agent methodology (Superpowers): what was taken, why, and how to use it |
| [`skynet-protocol.md`](skynet-protocol.md) | **Agent coordination protocol v1**: ranks/roles/specialization, registry + heartbeat + session-id reconciliation in MCP-memory, mailboxes (message format, TASK/REPORT/ACK flow), recovery procedure, protocol evolution |
| [`sourcecraft/README.md`](sourcecraft/README.md) | SourceCraft-specific: CI cubes/workflows, MCP tooling, PR automation |
| [`skills/refactor/`](../../skills/refactor/SKILL.md) | Agent skill: safe, behavior-preserving refactoring (Fowler catalog, smells, safety, test-gated steps); adapted from [`MuhiminOsim/code-refactoring-skill`](https://github.com/MuhiminOsim/code-refactoring-skill) (MIT) |
| [`skills/cpp/modern-cpp-en/`](../../skills/cpp/modern-cpp-en/SKILL.md) | Agent skill: Modern C++ (C++20/23/26) engineering guide (API design, error handling, concurrency, build acceleration, clang-tidy); adapted from [`huxint/cpp-standing-skill`](https://github.com/huxint/cpp-standing-skill) (MIT) |

## 5. Agent quick start

Depending on the task, start from:

- **Build / configure / test commands** → [`build.md`](build.md)
- **Where is module X / what does file Y do** → [`structure.md`](structure.md)
- **Who calls what / module interfaces** → [`module-interfaces.md`](module-interfaces.md)
- **How the engine works internally, invariants** → [`../en/deep-dive/architecture.md`](../en/deep-dive/architecture.md)
- **Debugging a slow/corrupt/growing DB, tools and scenarios** → [`debugging-methodology.md`](debugging-methodology.md)
- **How libmdbx works (functional view, RU, reader Q&A + deep dives)** → [`../ru/deep-dive/internals.md`](../ru/deep-dive/internals.md)
- **C++ API / classes / wrappers** → [`cxx-api.md`](cxx-api.md)
- **What tests guard a module / coverage gaps** → [`test-coverage.md`](test-coverage.md)
- **How to write tests / achieve full coverage (channels, axes)** → [`testing-methodology.md`](testing-methodology.md)
- **Operational test rules & CI budgets (tiers, flaky, SLA)** → [`testing-codex.md`](testing-codex.md)
- **Test scenarios as specs for test writers** → [`test-scenarios.md`](test-scenarios.md)
- **Managed fault injection / probing in tests** → [`probe-bus.md`](probe-bus.md), [`probes.md`](probes.md)
- **Critical invariants to check in reviews/tests** → [`libmdbx-invariants.md`](libmdbx-invariants.md), [`subsystem-map.md`](subsystem-map.md)
- **Subsystem conflict zones for parallel work** → [`module-lock-matrix.md`](module-lock-matrix.md)
- **Test durability modes / speed-up transports** → [`test-durability-strategy.md`](test-durability-strategy.md)
- **What to refactor first / known debt** → [`techdebt.md`](techdebt.md)
- **Refactoring / smells / cleanup workflow** → [`skills/refactor/`](../../skills/refactor/SKILL.md)
- **Writing or reviewing C++ (API layer)** → [`skills/cpp/modern-cpp-en/`](../../skills/cpp/modern-cpp-en/SKILL.md)
- **How to contribute / open PR / run CI** → [`workflows.md`](workflows.md), [`sourcecraft/README.md`](sourcecraft/README.md)
- **Feature design, planning, TDD, debugging** → [`skills/superpowers/README.md`](skills/superpowers/README.md)
- **Agent coordination (mailboxes, ranks, heartbeat, recovery)** → [`skynet-protocol.md`](skynet-protocol.md)

Common entry points in the code:

- Public C API surface: [`mdbx.h`](../../mdbx.h)
- Public C++ API surface: [`mdbx.h++`](../../mdbx.h++)
- Engine internals umbrella headers: [`src/internals.h`](../../src/internals.h), [`src/essentials.h`](../../src/essentials.h), [`src/preface.h`](../../src/preface.h), [`src/options.h`](../../src/options.h)
- Build options reference: [`src/options.h`](../../src/options.h) and `make options`
- Test registration: [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt)

## 6. Maintenance of this index

When the repository changes significantly (new modules, moved subsystems, changed build
targets), update the corresponding document. Keep the top-level map and the `src/`
classification in [`structure.md`](structure.md) in sync with the actual tree.