# CI Orchestrator — Requirements & Design

> Part of the [Skynet project index](README.md).
> Status: **authoritative specification** for `tests/ci/orchestrate.py` (the ci-dispatch
> orchestrator). Reconstructed from the documented goals (ci-dispatch.yml header, the
> orchestrator docstring, [workflows.md §4a](workflows.md), [testing-codex.md](testing-codex.md)
> C1–C4, [testing-infra-v3.md](testing-infra-v3.md), issue #224) and from the codified
> behavior of `tests/ci/test_orchestrate.py`. This document supersedes the phantom
> `.skynet/plans/testing-infra-v3-plan.md` pointer and resolves the contradictions listed
> in §8.

## 1. Purpose

ORCH-1. **Eliminate duplicate and pointless workflow runs and never lose coverage.**
The orchestrator mirrors "git describe over successful runs": the last successful
ci-dispatch run on the base branch is the diff anchor for cell selection; superseded or
history-rewritten runs on the current branch are cancelled; same-SHA duplicates keep
exactly one gate. *(orchestrate.py docstring; ci-dispatch.yml header)*

ORCH-2. **Sole ownership of cancellation.** The orchestrator is the only component that
cancels runs; ci-dispatch deliberately has no native `concurrency` group, because a
native cancel-in-progress group would blindly cancel queued/just-started runs on every
event — including runs the orchestrator intended to keep or skip. *(ci-dispatch.yml:23–26;
workflows.md §4a; testing-infra-v3.md §1)*

ORCH-3. **Change addressing.** "Do not run what has not changed": the changed-path set
selects the cells (testing-codex C1). The orchestrator delegates the path→cells math to
`tests/ci/select-cells.py` and never duplicates its policy.

## 2. Inputs

ORCH-4. Invocation contract (stable; consumed by ci-dispatch.yml resolve steps):
`orchestrate.py --repo R --workflow-id W --token T --head-ref B --head-sha S
--base-ref BR --event {pull_request|push|schedule|workflow_dispatch|repository_dispatch}
--run-id N [--dry-run]`.

ORCH-5. The caller passes bare branch names in `--head-ref` (the workflow normalizes
`github.head_ref || github.ref_name`); the orchestrator strips a `refs/heads/` prefix
defensively, because the Actions API `branch=` filter silently matches nothing with the
prefix.

ORCH-6. `--run-id` identifies this run's own entry (`github.run_id`, a string) and must
be exempt from dedup and cancellation. Ids are compared as strings: the API returns int
ids, and a mixed comparison once disabled the exemption entirely (the run deduplicated
and cancelled against itself).

ORCH-7. An empty `--run-id` disables the own-run exemption; an empty `--head-ref`
(bare `repository_dispatch`) disables supersede/dedup entirely — an unfiltered run
listing would see other branches and could cancel or skip on foreign runs.

## 3. Coverage guarantees (invariants)

INV-1. **Never green with an empty matrix.** `skip=true` is emitted only when coverage
is provably provided elsewhere: a live or successful same-SHA duplicate, a stale self-skip
(the current-tip run owns the gate), or a preserved-only run (literal `[]` with
non-empty `preserved`). `cells == ""` is emitted only together with `skip=true`.

INV-2. **No anchor → full gate.** With no successful base run in the ancestry of HEAD
(within the anchor window), the selection is the full profile.

INV-3. **Fail-safe.** Any internal error degrades to: `skip=false`, no cancellations,
full-gate cells — coverage is never silently reduced. The orchestrator always exits 0
after emitting its JSON (argparse usage errors exit 2); the docstring's historical
"exit 1 = fatal" is retired (contradiction C5).

INV-4. **Preservation coverage.** Preserved in-flight cells plus the spawned complement
cover the new selection: `spawn ∪ preserved_cells ⊇ selection` for chains of preserved
runs. When nothing new is spawned, `cells` is the literal `[]` (the run job is skipped,
`dist` still runs) — an empty string stays reserved for the skip signal.

## 4. Anchoring & selection

ORCH-8. The diff anchor is the nearest successful base run: successful runs of the base
branch are tagged `ci-ok-<run_number>` (temporary, local, always cleaned up in
`finally`), and the tag whose SHA is an ancestor of HEAD with the minimal
`rev-list --count` distance wins.

ORCH-9. Anchor search window: successful runs are queried with a `created` window of
the last 30 days (`ANCHOR_MAX_AGE_DAYS`) — older SHAs are routinely rewritten away on
this repository and cannot be tagged; a SHA missing from the checkout is skipped with a
single summary line, not a per-SHA error dump.

ORCH-10. Base candidates: the requested base first, then `origin/devel` as fallback (a
feature branch may branch off devel). The first candidate yielding an ancestor anchor wins.

ORCH-11. Selection delegates to `tests/ci/select-cells.py` (`--base <anchor>`; on no
anchor `--profile full`). Any non-zero exit is a fatal signal handled by INV-3.

ORCH-12. Tests-changed escalation: a diff touching `tests/` extends the scope to
`fast+hygiene` (the hygiene check guards against leftover database files). Implemented
in the workflow resolve step, not in the orchestrator.

## 5. Same-SHA dedup — the PR-wins matrix

ORCH-13. For each listed run of the head branch with `head_sha == this head_sha` and
`id != this run id`:

| this event | twin event | twin state | verdict |
|---|---|---|---|
| pull_request | push | alive | **cancel the push run, proceed** (PR wins) |
| pull_request | push | success | skip (coverage provided) |
| pull_request | push | failure/other | proceed (the gate left empty; provide the signal) |
| push | pull_request | alive or success | skip (the PR gate is authoritative) |
| push | pull_request | failure/other | proceed |
| same event | — | alive or success | skip (conservative) |
| any | any | cancelled | neutral — proceed (a cancelled run produced no result) |

ORCH-14. A cancelled same-SHA run must never cause a skip: it produced no result, and
skipping on top of it would leave the gate empty.

ORCH-15. Historical note: the pre-rewrite dedup skipped *each* run as a duplicate of
the other, so simultaneous push+PR start degenerated into a mutual-skip deadlock — a
green PR with the matrix never running (observed: the MSVC C7757 failure of the gh0452
fix reached master through it). The PR-wins matrix above replaces that behavior.

## 6. Supersession & preservation

ORCH-16. An unfinished run whose head is an ancestor of this run's head is **superseded**
by linear history → cancel (`kind = superseded`).

ORCH-17. An unfinished run whose head is **not** an ancestor (a force-push invalidated
its lineage) → cancel (`kind = rewritten-history`).

ORCH-18. **Preserve-unaffected supersede (issue #224, coarse model).** A superseded run
whose follow-up diff (`run_sha...head_sha`, three-dot) touches **no** `common_prefixes`
file stays alive: its in-flight cells are provably unaffected. The new run spawns only
the complement of the preserved selection, estimated with the same two-tier trigger
model (`cells_for_paths` over `git diff base...run_sha`). Requirements: the run has a
base anchor. The coverage invariant INV-4 holds for chains of preserved runs.

ORCH-19. Per-cell dependency modeling is a deliberate non-goal until the registry gains
a cell→paths map (the coarse two-tier model is a conservative superset).

ORCH-20. Only unfinished runs (`in_progress`, `queued`, `pending`, `waiting`) are
cancellable. This run's own entry is never a cancel candidate. Cancellation failures are
logged and non-fatal; `cancel_ids` records **confirmed** cancellations only.

## 7. Staleness

ORCH-21. Before touching anything, the orchestrator reads the branch tip
(`GET branches/<head_ref>`). When the tip differs from this run's head SHA, the run is
**stale** (it sat queued while the branch moved on): it skips itself — the current-tip
run owns supersede/dedup — and cancels nothing. A stale run that would classify the
current tip as "rewritten history" must not kill the authoritative gate.

ORCH-22. If the branch-tip lookup fails, no verdict is possible and the previous
behavior is kept ("assuming this run is current").

ORCH-25. **The push leg defers to an open pull request for its branch.** A
branch push with an open PR fires both gates at the same moment; under the
PR-wins priority the PR run would cancel the push run, surfacing red
cancelled checks on the PR. Instead, the push run checks for an open PR on
its branch and skips itself green — the PR gate covers the same SHA. A
failed open-PR lookup keeps the previous behavior. (INV-1: the coverage is
provided by the PR gate.)

## 8. Contradictions resolved (from the source inventory)

| # | Contradiction | Resolution |
|---|---|---|
| C1 | ci-dispatch header mentions `CI_PUSH_CAP`, which exists nowhere | header comment corrected (the cap is `select-cells --cap N`, currently unused on push) |
| C2 | header says push = "diff vs previous commit, branches devel/master"; actual = any branch, orchestrated diff vs the nearest successful base | header rewritten to the shipped semantics |
| C3 | cell counts differ across docs (5/7, 148/24, 186/190/192) | counts are stated only in [testing-infra-v3.md](testing-infra-v3.md) §2, kept in sync with the registry |
| C4 | v2 design doc describes merge-base diff and a dispatcher-owned schedule | v2 doc is explicitly labeled background; this document defines the shipped semantics |
| C5 | docstring "exit 1 = fatal" vs always-exit-0 fail-safe | exit contract redefined: always 0 after JSON; argparse usage errors exit 2 |
| C6 | skynet `fact:testing:ci-state` describes v2 | knowledge record to be updated separately (memory-codex procedure, not this document) |

## 9. Decision flow (normative)

1. Token gate: empty token → INV-3 fail-safe.
2. Base anchor loop over candidates (ORCH-8..10): tag successful runs → nearest
   ancestor → break on first hit.
3. Branch normalization (ORCH-5); empty head_ref → skip steps 4–5.
4. Run-level self-skip gates: staleness (ORCH-21..22) and the push leg's
   open-PR deferral (ORCH-25) → `skip=true`, no listing, no cancellations.
5. Head-branch run listing (paginated; no event/status filter): classify every run —
   same-SHA dedup (ORCH-13), finished runs are not cancellable, own entry exempt,
   supersede/rewritten/preserve per §6.
6. Cell selection: skip → `""`; preserved → spawn complement (or literal `[]`);
   anchor → `--base`; no anchor → `--profile full`.
7. Fail-safe wrap (INV-3); `finally` cleanup of `ci-ok-*` tags; one JSON line on
   stdout; exit 0.

## 10. Observability

ORCH-23. All decisions go to stderr with the `orchestrate: ` prefix (anchor verdict,
stale skip, duplicates, supersessions, preservations, preserve/spawn/duplicated
metrics, selection counts, dead-SHA summary, fatal). The workflow echoes stdout only;
stderr is for humans and the CI log.

ORCH-24. `--dry-run` prints the would-be plan (tags, cancellations, cleanup) without
touching anything.

## 11. Requirements → tests traceability

Every behavioral requirement carries a test in `tests/ci/test_orchestrate.py` (fake
Actions-API harness over real git objects, per [testing-infra-v3.md](testing-infra-v3.md)
§"Extending"). The rewrite keeps a 1:1 mapping of the pre-existing tests and adds
coverage for the previously untested policies: cancelled-twin neutrality, failed-twin
rerun, fail-safe path, empty-token gate, base-candidate fallback, `cleanup_tags`
placement, `--dry-run`, partial-cancel accounting, branch-tip lookup failure, multiple
preserved runs, `cells_for_paths` tiers, `select_cells` failure → full gate, three-dot
diff semantics, empty run-id edge, stale output shape, listing pagination. The
combination matrix (event × twin status × twin conclusion × stale × anchor) asserts
INV-1 exhaustively.
