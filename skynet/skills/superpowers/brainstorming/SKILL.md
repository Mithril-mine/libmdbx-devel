---
name: brainstorming
description: Use before writing code or making design decisions, to refine rough ideas into a validated design through questions and incremental presentation
---

# Brainstorming

*Adapted from obra/superpowers (MIT). Use when a task is vague, large, or has design trade-offs —
especially for engine internals, API changes, or test-infrastructure work in libmdbx.*

## Purpose

Turn a rough idea into a **design document** that a plan can be built from — before any code is
written. Exploration happens here, in conversation, not in the codebase.

## When to use

- New features or behavior changes (API, build/test infrastructure, engine internals).
- Bug fixes where the *right* fix is unclear.
- Any task with meaningful alternatives or risk.

## Process

### 1. Understand the real goal

Ask questions until the underlying problem is clear. Do not accept the first framing.
Useful prompts:

- "What is the user-visible outcome we need?"
- "What constraints exist (performance, memory, disk format, portability, amalgamation)?"
- "Who consumes this (public C/C++ API, mdbx_chk, tests, build system)?"
- "What could go wrong, and what does failure look like?"

For libmdbx, always consider the hard constraints first — see
[`../../../architecture.md`](../../../architecture.md) §8 invariants (disk format is frozen, readers must
stay wait-free, TLS destructor contract, page-state machine).

### 2. Explore alternatives

Present 2–4 options with trade-offs. Do not hide the ugly ones. For example, when changing
tests: extend `mdbx_test` framework vs add standalone CTest tests vs integrate GoogleTest —
each has cost/benefit for the stochastic vs deterministic split.

### 3. Present the design in digestible sections

Show the design incrementally (overview → data structures → flow → edge cases → testing), and
pause after each part for validation. If the user pushes back, adapt — do not defend blindly.

### 4. Record the outcome

Save the agreed design as a short markdown document (e.g. under `skynet/` or a design notes
file) so the subsequent `writing-plans` skill can reference it. The document should contain:
goal, non-goals, chosen approach, key trade-offs, affected modules/files, testing strategy.

## Output

A concise design document + agreement on scope. **No implementation code yet.**

## Project notes (libmdbx)

- Reference materials: [`skynet/`](../README.md) index (structure/architecture/build/workflows),
  `docs/`, `ChangeLog*.md`, upstream <https://libmdbx.dqdkfa.ru>.
- Design docs must respect the **dist-cutoff / amalgamation** rules if they touch build files
  (see [`../../../build.md`](../../../build.md) §7) and the LLVM code style.
- New public API surface requires updating `mdbx.h`/`mdbx.h++` docs and usually a
  `tests/issues/issue_ghNNNN.c++` regression.