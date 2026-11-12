#!/usr/bin/env python3
# Infra v3 (C2): run orchestrator for ci-dispatch.
#
# Requirements & design: docs/engineering/ci-orchestrator-requirements.md —
# the authoritative specification (ORCH-*/INV-* references in the comments
# below point there). This file is the implementation; the summary:
#
# Goal: eliminate duplicate/pointless workflow runs and never lose coverage:
# the last successful ci-dispatch run on the base branch anchors the diff for
# cell selection; superseded/rewritten runs on the current branch are
# cancelled; same-SHA duplicates keep exactly one gate (pull_request outranks
# push); a follow-up diff touching no common-prefix file preserves the
# superseded run in flight (issue #224).
#
# Architecture: a pure planning core (`RunInfo`, `same_sha_verdict`, `plan`)
# decides everything from injected inputs; `main()` is the effects layer
# (GitHub API, git tags, the select-cells subprocess) feeding the core and
# applying its decision. The external contract is unchanged: the CLI
# arguments, the stdout JSON {"base_sha", "cells", "skip", "cancel_ids",
# "preserved", "preserved_cells"} and stderr diagnostics. The orchestrator
# always exits 0 after emitting its JSON (INV-3; argparse usage errors exit
# 2) — the historical "exit 1 = fatal" contract is retired.

import argparse
import datetime
import importlib.util
import json
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TAG_PREFIX = "ci-ok-"
REGISTRY = REPO_ROOT / "tests" / "ci" / "config.json"
# Anchor search window: successful runs older than this are almost always
# rewritten away on this repo (history rewrites are routine), so their shas
# cannot be tagged and would only pollute the search (ORCH-9).
ANCHOR_MAX_AGE_DAYS = 30
# Run states that can/need to be cancelled (ORCH-20).
ALIVE_STATES = ("in_progress", "queued", "pending", "waiting")


def _load_select_cells():
    """Import tests/ci/select-cells.py (the dash blocks a plain import)."""
    spec = importlib.util.spec_from_file_location(
        "select_cells_mod", Path(__file__).resolve().parent / "select-cells.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sc = _load_select_cells()  # hits_common / change_addressed_cells / profile_cells


# ----------------------------------------------------------------------------
# Effects primitives (I/O; injected/mocked in tests)
# ----------------------------------------------------------------------------

def git(args):
    return subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                          capture_output=True, text=True)


def api(repo, path, token, method="GET", payload=None):
    url = f"https://api.github.com/repos/{repo}/{path.lstrip('/')}"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        print(f"orchestrate: api {method} {path} -> HTTP {e.code}: {e.read().decode()[:300]}",
              file=sys.stderr)
        raise


def list_runs(repo, token, workflow_id, branch=None, event=None, status=None):
    """All matching runs, pagination followed (the previous single-page
    listing silently truncated at 100 entries)."""
    runs = []
    page = 1
    while True:
        path = f"actions/workflows/{workflow_id}/runs?per_page=100&page={page}"
        if branch:
            path += f"&branch={urllib.parse.quote(branch)}"
        if event:
            path += f"&event={event}"
        if status:
            path += f"&status={status}"
            if status == "success":
                # Anchor window (ORCH-9): ancient successful runs are
                # routinely rewritten away and would only pollute the search.
                since = (datetime.datetime.now(datetime.timezone.utc)
                         - datetime.timedelta(days=ANCHOR_MAX_AGE_DAYS)).strftime("%Y-%m-%d")
                path += f"&created={since}..*"
        data = api(repo, path, token)
        chunk = data.get("workflow_runs", [])
        runs.extend(chunk)
        if len(chunk) < 100:
            return runs
        page += 1


def cancel_run(repo, token, run_id, dry_run) -> bool:
    """POST the cancellation. Returns True when confirmed (or dry-run);
    failures are logged and reported, not fatal (ORCH-20)."""
    if dry_run:
        print(f"orchestrate: [dry-run] would cancel run {run_id}", file=sys.stderr)
        return True
    try:
        api(repo, f"actions/runs/{run_id}/cancel", token, method="POST")
        return True
    except Exception as e:  # noqa: BLE001 — a failed cancel is logged, not fatal
        print(f"orchestrate: cancel {run_id} failed: {e}", file=sys.stderr)
        return False


def tag_successful_runs(successful, dry_run):
    """Create temporary lightweight tags ci-ok-<run_number> on successful base
    runs (ORCH-8); SHAs rewritten away are skipped with a summary line
    (ORCH-9)."""
    made = []
    dead = 0
    for run in successful:
        tag = f"{TAG_PREFIX}{run['run_number']}"
        sha = run["head_sha"]
        if dry_run:
            made.append((tag, sha))
            continue
        if git(["cat-file", "-e", f"{sha}^{{commit}}"]).returncode != 0:
            # rewritten away (routine on this repo): it cannot anchor the
            # diff — skip quietly, one summary line at the end (ORCH-9).
            dead += 1
            continue
        r = git(["tag", tag, sha])
        if r.returncode != 0:
            print(f"orchestrate: tag {tag} failed: {r.stderr.strip()}", file=sys.stderr)
            continue
        made.append((tag, sha))
    if dead:
        print(f"orchestrate: skipped {dead} successful-run shas missing from the "
              f"checkout (rewritten history)", file=sys.stderr)
    return made


def nearest_ancestor_sha():
    """Find the closest successful base-run SHA that is an ancestor of HEAD
    (ORCH-8): among the ci-ok-* tags keep the ancestors and pick the minimal
    rev-list distance. Deterministic and merge-safe (git describe would
    prefer by tag naming, not topology). Returns (sha, distance) or
    (None, None)."""
    tags = git(["tag", "-l", f"{TAG_PREFIX}*"]).stdout.splitlines()
    best = (None, None)
    for tag in tags:
        sha = git(["rev-list", "-n", "1", tag]).stdout.strip()
        if not sha:
            continue
        anc = git(["merge-base", "--is-ancestor", sha, "HEAD"])
        if anc.returncode != 0:
            continue
        dist = git(["rev-list", "--count", f"{sha}..HEAD"]).stdout.strip()
        try:
            dist = int(dist)
        except ValueError:
            continue
        if best[1] is None or dist < best[1]:
            best = (sha, dist)
    return best


def cleanup_tags(dry_run):
    tags = git(["tag", "-l", f"{TAG_PREFIX}*"]).stdout.splitlines()
    if dry_run:
        for t in tags:
            print(f"orchestrate: [dry-run] would delete tag {t}", file=sys.stderr)
        return
    if tags:
        git(["tag", "-d", *tags])


def normalize_branch(ref):
    """Bare branch name for the Actions API `branch=` filter (ORCH-5).

    Push events carry the full ref (`refs/heads/<branch>`); the API expects
    the bare name and silently matches nothing otherwise — which used to
    disable supersede/dedup on every push.
    """
    if ref.startswith("refs/heads/"):
        return ref[len("refs/heads/"):]
    return ref


def diff_paths(a, b):
    """Files changed a...b (three-dot, the same form select-cells uses)."""
    return [p for p in git(["diff", "--name-only", f"{a}...{b}"]).stdout.splitlines() if p]


def cells_for_paths(paths):
    """Two-tier trigger model over an explicit path list (select-cells logic,
    used to estimate a preserved run's selection; ORCH-18).

    common_prefixes hit -> change-addressed gate (push-quick ∪ win-gate),
    otherwise push-quick.
    """
    if not paths:
        return []
    cfg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    if sc.hits_common(cfg, paths):
        return sc.change_addressed_cells(cfg)
    return sc.profile_cells(cfg, "push-quick") or []


def select_cells(base_sha=None, profile=None):
    """Run tests/ci/select-cells.py and return newline-joined cell ids.

    Raises RuntimeError on failure (caller decides the fallback). This is a
    seam: tests monkeypatch it instead of spawning the subprocess (ORCH-3,
    ORCH-11).
    """
    cmd = [sys.executable, str(REPO_ROOT / "tests/ci/select-cells.py")]
    if profile:
        cmd += ["--profile", profile]
    else:
        cmd += ["--base", base_sha]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    if r.returncode != 0:
        print(f"orchestrate: select-cells failed: {r.stderr.strip()}", file=sys.stderr)
        raise RuntimeError(f"select-cells failed (profile={profile}, base={base_sha})")
    return "\n".join(l for l in r.stdout.splitlines() if l)


# ----------------------------------------------------------------------------
# Planning core (pure: no I/O — inputs and probes are injected, decisions
# are data; ORCH-13..ORCH-22 and the invariants INV-1..INV-4 are normative
# here)
# ----------------------------------------------------------------------------

@dataclass(frozen=True)
class RunInfo:
    """A normalized Actions run entry. Ids are strings: github.run_id arrives
    as a string while the API returns int ids, and a mixed comparison once
    disabled the own-run exemption entirely (ORCH-6)."""
    id: str
    head_sha: str
    status: str
    conclusion: Optional[str]
    event: str

    @classmethod
    def from_api(cls, raw: dict) -> "RunInfo":
        return cls(id=str(raw["id"]), head_sha=raw["head_sha"], status=raw["status"],
                   conclusion=raw.get("conclusion"), event=raw.get("event", ""))

    @property
    def alive(self) -> bool:
        return self.status in ALIVE_STATES


# Same-SHA dedup verdicts (ORCH-13): what THIS run does about the twin.
SUPERSEDE = "supersede"   # cancel the twin, run the gate (PR wins)
DEFER = "defer"           # skip self (the twin is authoritative)
NEUTRAL = "neutral"       # the twin produced no result; this run proceeds


def same_sha_verdict(this_event: str, twin: RunInfo) -> str:
    """The PR-wins matrix (ORCH-13/14)."""
    if twin.conclusion == "cancelled":
        return NEUTRAL
    ok = twin.conclusion == "success"
    pair = (this_event, twin.event)
    if pair == ("pull_request", "push"):
        if ok:
            return DEFER
        return SUPERSEDE if twin.alive else NEUTRAL
    if pair == ("push", "pull_request"):
        return DEFER if (ok or twin.alive) else NEUTRAL
    return DEFER  # same-event duplicate: conservative (ORCH-13, last row)


@dataclass
class Decision:
    """The gate plan (pure data; main() applies it)."""
    skip: bool = False
    cells: str = ""
    base_sha: str = ""
    cancel_ids: List[str] = None
    preserved: List[str] = None
    preserved_cells: List[str] = None

    def __post_init__(self):
        self.cancel_ids = self.cancel_ids if self.cancel_ids is not None else []
        self.preserved = self.preserved if self.preserved is not None else []
        self.preserved_cells = self.preserved_cells if self.preserved_cells is not None else []


def plan(event: str, head_sha: str, run_id: str, runs: List[RunInfo],
         skip_self: bool, base_sha: Optional[str],
         is_ancestor: Callable[[str], bool],
         followup_touches_common: Callable[[str], bool],
         estimate_cells: Callable[[str], List[str]],
         select_gate: Callable[[], str]) -> Decision:
    """The pure planning core: decide the gate from the injected inputs.

    runs — the head-branch listing (RunInfo); skip_self — this run must not
    run the gate (the coverage is provided elsewhere: the branch tip moved on
    past head_sha, ORCH-21, or an open pull request covers the same SHA,
    ORCH-25); is_ancestor(sha) — linear-history test;
    followup_touches_common(run_sha) — whether the diff run_sha...head_sha
    touches a common_prefixes file (ORCH-18); estimate_cells(run_sha) — the
    preserved run's estimated selection; select_gate() — the new selection
    (diff vs the base anchor, or the full gate).
    """
    decision = Decision()
    if skip_self:
        # ORCH-21/25: the current-tip run (or the PR gate) owns
        # supersede/dedup; this run must neither cancel it nor run a gate
        # for the content it does not own.
        decision.skip = True
        decision.cells = ""
        return decision

    preserved_cells: List[str] = []

    for run in runs:
        if run.id == run_id:
            continue  # own-run exemption (ORCH-6)
        if run.head_sha == head_sha:
            verdict = same_sha_verdict(event, run)
            if verdict == SUPERSEDE:
                print(f"orchestrate: superseding the live {run.event} run {run.id} "
                      f"on the same SHA (PR wins)", file=sys.stderr)
                decision.cancel_ids.append(run.id)
            elif verdict == DEFER:
                print(f"orchestrate: duplicate {run.event} run {run.id} "
                      f"on the same SHA; skip this {event} run", file=sys.stderr)
                decision.skip = True
                break  # the twin is authoritative: nothing else matters
            else:
                print(f"orchestrate: same-SHA {run.event} run {run.id} is "
                      f"{run.conclusion or run.status}; proceeding", file=sys.stderr)
            continue
        if not run.alive:
            continue  # only unfinished runs are cancellable (ORCH-20)
        anc = is_ancestor(run.head_sha)
        kind = "superseded" if anc else "rewritten-history"
        # Preserve-unaffected supersede (ORCH-18, issue #224, coarse model):
        # a superseded run stays alive when the follow-up diff touches no
        # common_prefixes file — its in-flight cells are provably unaffected,
        # and this run only spawns the complement. Any common-code change (or
        # a rewritten-history run, or a run without a base anchor) still
        # cancels-and-reruns as before.
        if anc and base_sha and not followup_touches_common(run.head_sha):
            est = estimate_cells(run.head_sha)
            decision.preserved.append(run.id)
            for cid in est:
                if cid not in preserved_cells:
                    preserved_cells.append(cid)
            print(f"orchestrate: preserving {run.event} run {run.id} "
                  f"({run.head_sha[:12]}): follow-up diff touches no "
                  f"common code, ~{len(est)} cells stay in flight", file=sys.stderr)
            continue
        print(f"orchestrate: cancelling {kind} {run.event} run {run.id} "
              f"({run.head_sha[:12]})", file=sys.stderr)
        decision.cancel_ids.append(run.id)

    decision.preserved_cells = preserved_cells
    decision.base_sha = base_sha or ""

    # Cell selection (INV-1: skip ⇒ coverage provided elsewhere; INV-4).
    if decision.skip:
        decision.cells = ""
        return decision
    if base_sha:
        selected = [l for l in select_gate().splitlines() if l]
        if preserved_cells:
            spawn = [c for c in selected if c not in set(preserved_cells)]
            print(f"orchestrate: preserve metrics: selected {len(selected)}, "
                  f"spawn {len(spawn)}, duplicated {len(selected) - len(spawn)} "
                  f"(kept in-flight runs: {len(decision.preserved)})", file=sys.stderr)
            # literal [] keeps the run job skipped while the preserved runs
            # carry the coverage; an empty string would read as the
            # fatal-selection signal instead (INV-4).
            decision.cells = "\n".join(spawn) if spawn else "[]"
        else:
            decision.cells = "\n".join(selected)
            print(f"orchestrate: selected {len(selected)} cells "
                  f"(diff vs {base_sha[:12]})", file=sys.stderr)
    else:
        decision.cells = select_gate()
        print(f"orchestrate: selected full gate ({len(decision.cells.splitlines())} cells)",
              file=sys.stderr)

    # INV-1: a skip always carries its coverage reason; an empty cells string
    # is only ever emitted together with skip (never-green-empty).
    assert decision.skip or decision.cells, "INV-1 violated: empty gate without skip"
    return decision


# ----------------------------------------------------------------------------
# Effects layer
# ----------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(add_help=False)
    # argv=None -> sys.argv (script use); a list -> injected (tests).
    parser.add_argument("--repo", required=True)
    parser.add_argument("--workflow-id", required=True)
    parser.add_argument("--token", default="")
    parser.add_argument("--head-ref", required=True)
    parser.add_argument("--head-sha", required=True)
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--event", required=True)
    parser.add_argument("--run-id", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    out = {"base_sha": "", "cells": "", "skip": False, "cancel_ids": [],
           "preserved": [], "preserved_cells": []}
    try:
        if not args.token:
            raise RuntimeError("GITHUB_TOKEN is empty")

        # 1. successful base runs -> temporary tags (ORCH-8..10). Try the
        #    requested base first, then origin/devel as a fallback: a feature
        #    branch may branch off devel rather than master, so the "main"
        #    base may not be an ancestor even though devel is.
        base_candidates = [args.base_ref]
        if args.base_ref not in ("origin/devel", "devel"):
            base_candidates.append("origin/devel")
        base_sha = None
        for base_ref in base_candidates:
            base_branch = base_ref
            if base_branch.startswith("origin/"):
                base_branch = base_branch[len("origin/"):]
            ok_runs = list_runs(args.repo, args.token, args.workflow_id,
                                branch=base_branch, event="push", status="success")
            tag_successful_runs(ok_runs, args.dry_run)
            base_sha, _dist = nearest_ancestor_sha()
            if base_sha:
                break
        # 2. nearest successful ancestor as the diff anchor (ORCH-8; no
        #    anchor -> full gate, INV-2).
        if base_sha:
            out["base_sha"] = base_sha
            print(f"orchestrate: base anchor = {base_sha[:12]} (nearest successful base run)",
                  file=sys.stderr)
        else:
            print("orchestrate: no successful base run in ancestry -> full gate",
                  file=sys.stderr)

        # 3. run-level self-skip gates (ORCH-21/22, ORCH-25):
        #    - staleness: a queued run may wake after the branch tip moved on;
        #      it must neither cancel the current lineage nor run a gate for
        #      the abandoned content;
        #    - an open pull request for this branch (push leg only): the PR
        #      gate covers the same SHA, and running both would end in this
        #      run being cancelled by the PR run - surfacing red cancelled
        #      checks on the PR. Deferring keeps the checks green.
        head_ref = normalize_branch(args.head_ref)
        stale = False
        if head_ref:
            try:
                tip = api(args.repo,
                          f"branches/{urllib.parse.quote(head_ref)}",
                          args.token)["commit"]["sha"]
                stale = tip != args.head_sha
            except Exception as e:  # noqa: BLE001 — no verdict, keep old behavior (ORCH-22)
                print(f"orchestrate: branch-tip lookup failed ({e}); "
                      f"assuming this run is current", file=sys.stderr)
            if stale:
                print(f"orchestrate: stale run: branch {head_ref} tip is "
                      f"{tip[:12]}, not {args.head_sha[:12]}; skipping the gate",
                      file=sys.stderr)
        deferred_to_pr_gate = False
        if args.event == "push" and head_ref and not stale:
            try:
                open_prs = api(args.repo,
                               f"pulls?head={urllib.parse.quote(args.repo)}:"
                               f"{urllib.parse.quote(head_ref)}&state=open",
                               args.token)
                deferred_to_pr_gate = bool(open_prs)
            except Exception as e:  # noqa: BLE001 — no verdict, keep old behavior
                print(f"orchestrate: open-PR lookup failed ({e}); "
                      f"assuming there is none", file=sys.stderr)
            if deferred_to_pr_gate:
                print(f"orchestrate: an open pull request exists for {head_ref}; "
                      f"deferring to the PR gate", file=sys.stderr)

        # 4. plan (pure core) from the gathered inputs.
        runs = []
        if head_ref and not stale and not deferred_to_pr_gate:
            runs = [RunInfo.from_api(r) for r in
                    list_runs(args.repo, args.token, args.workflow_id, branch=head_ref)]

        registry_cfg = json.loads(REGISTRY.read_text(encoding="utf-8"))

        def followup_touches_common(run_sha):
            return sc.hits_common(registry_cfg, diff_paths(run_sha, args.head_sha))

        def estimate_cells(run_sha):
            return cells_for_paths(diff_paths(base_sha, run_sha)) if base_sha else []

        def select_gate():
            if base_sha:
                return select_cells(base_sha=base_sha)
            return select_cells(profile="full")

        decision = plan(event=args.event, head_sha=args.head_sha,
                        run_id=str(args.run_id), runs=runs,
                        skip_self=stale or deferred_to_pr_gate,
                        base_sha=base_sha,
                        # the ancestry is measured against the planning head
                        # (args.head_sha) — the checked-out HEAD is only how
                        # CI happens to materialize it
                        is_ancestor=lambda sha: git(
                            ["merge-base", "--is-ancestor", sha,
                             args.head_sha]).returncode == 0,
                        followup_touches_common=followup_touches_common,
                        estimate_cells=estimate_cells, select_gate=select_gate)

        # 5. apply the cancellations; report confirmed ones only (ORCH-20).
        for rid in decision.cancel_ids:
            if cancel_run(args.repo, args.token, rid, args.dry_run):
                out["cancel_ids"].append(rid)

        out.update(skip=decision.skip, cells=decision.cells,
                   preserved=decision.preserved, preserved_cells=decision.preserved_cells)
        if decision.skip:
            out["cells"] = ""
    except Exception as e:
        print(f"orchestrate: fatal: {e}", file=sys.stderr)
        # Fail-safe (INV-3): report skip=false, no cancellations, and request
        # the full gate so coverage is never silently reduced.
        try:
            cells = select_cells(profile="full")
        except Exception as e2:  # noqa: BLE001 — fail-safe must not raise
            print(f"orchestrate: full-gate selection failed too: {e2}", file=sys.stderr)
            cells = ""
        out = {"base_sha": "", "cells": cells, "skip": False, "cancel_ids": [],
               "preserved": [], "preserved_cells": []}
    finally:
        cleanup_tags(args.dry_run)

    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
