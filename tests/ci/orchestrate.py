#!/usr/bin/env python3
# Infra v3 (C2): run orchestrator for ci-dispatch.
#
# Goal: eliminate duplicate/pointless workflow runs and avoid losing coverage
# when runs are cancelled, by mirroring "git describe over successful runs":
# the last successful ci-dispatch run on the base branch is used as the diff
# anchor for cell selection, superseded/rewritten runs on the current branch
# are cancelled, and a push/pull_request duplicate on the same SHA keeps only
# the higher-priority run.
#
# Data sources (all via the GitHub Actions/Checks API with GITHUB_TOKEN):
#   * successful runs on the base branch  -> git tags ci-ok-<run_number>
#   * runs on the head branch             -> cancel candidates
#   * open pull requests for the head ref -> priority (pull_request > push)
#
# Exit codes: 0 = ok, 1 = fatal error (caller should fall back to full).
# Output (stdout, JSON):
#   {"base_sha": "<sha or ''>", "cells": "<ids>", "skip": bool,
#    "cancel_ids": [ids], "preserved": [ids], "preserved_cells": [ids]}
#   cells may be empty only when skip=true (duplicate superseded by another
#   run on the same SHA); otherwise empty cells is a fatal signal. The
#   literal "[]" is emitted when preserved runs keep the whole selection in
#   flight and nothing new is spawned (the run job is skipped, dist still
#   runs). "preserved" lists the kept runs: superseded runs whose follow-up
#   diff touches no common_prefixes file (issue #224, coarse model) — their
#   in-flight cells are provably unaffected, and preserved_cells estimates
#   their selection so the new run spawns only the complement.
#
# --dry-run prints the plan without tagging/cancelling.

import argparse
import datetime
import importlib.util
import json
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TAG_PREFIX = "ci-ok-"
REGISTRY = REPO_ROOT / "tests" / "ci" / "config.json"
# Anchor search window: successful runs older than this are almost always
# rewritten away on this repo (history rewrites are routine), so their shas
# cannot be tagged and would only pollute the search.
ANCHOR_MAX_AGE_DAYS = 30


def _load_select_cells():
    """Import tests/ci/select-cells.py (the dash blocks a plain import)."""
    spec = importlib.util.spec_from_file_location(
        "select_cells_mod", Path(__file__).resolve().parent / "select-cells.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sc = _load_select_cells()  # hits_common / change_addressed_cells / profile_cells


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


def list_runs(repo, token, workflow_id, branch=None, event=None, status=None, per_page=100):
    path = f"actions/workflows/{workflow_id}/runs?per_page={per_page}"
    if branch:
        path += f"&branch={urllib.parse.quote(branch)}"
    if event:
        path += f"&event={event}"
    if status:
        path += f"&status={status}"
        if status == "success":
            since = (datetime.datetime.now(datetime.timezone.utc)
                     - datetime.timedelta(days=ANCHOR_MAX_AGE_DAYS)).strftime("%Y-%m-%d")
            path += f"&created={since}..*"
    data = api(repo, path, token)
    return data.get("workflow_runs", [])


def cancel_runs(repo, token, run_ids, dry_run):
    cancelled = []
    for rid in run_ids:
        if dry_run:
            print(f"orchestrate: [dry-run] would cancel run {rid}", file=sys.stderr)
            cancelled.append(rid)
            continue
        try:
            api(repo, f"actions/runs/{rid}/cancel", token, method="POST")
            cancelled.append(rid)
        except Exception as e:
            print(f"orchestrate: cancel {rid} failed: {e}", file=sys.stderr)
    return cancelled


def tag_successful_runs(successful, dry_run):
    """Create temporary lightweight tags ci-ok-<run_number> on successful base runs."""
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
            # diff — skip quietly, one summary line at the end.
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
    """Find the closest successful base-run SHA that is an ancestor of HEAD.

    Explicit search: for each tag ci-ok-*, check merge-base --is-ancestor and
    pick the one with the minimal rev-list distance to HEAD. Deterministic and
    merge-safe (git describe would prefer by tag naming, not topology).
    Returns (sha, distance) or (None, None).
    """
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
    """Bare branch name for the Actions API `branch=` filter.

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
    """Two-tier trigger model over an explicit path list (select-cells logic).

    common_prefixes hit -> change-addressed gate (push-quick ∪ win-gate),
    otherwise push-quick. Used to estimate a preserved run's selection.
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
    seam: tests monkeypatch it instead of spawning the subprocess.
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

        # 1. successful base runs -> temporary tags
        # Try the requested base first, then origin/devel as a fallback: a
        # feature branch may branch off devel rather than master, so the
        # "main" base may not be an ancestor even though devel is.
        base_candidates = [args.base_ref]
        if args.base_ref not in ("origin/devel", "devel"):
            base_candidates.append("origin/devel")
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
        # 2. nearest successful ancestor as the diff anchor
        if base_sha:
            out["base_sha"] = base_sha
            print(f"orchestrate: base anchor = {base_sha[:12]} (nearest successful base run)",
                  file=sys.stderr)
        else:
            print("orchestrate: no successful base run in ancestry -> full gate",
                  file=sys.stderr)

        # 3. head-branch runs: cancel in-progress superseded/rewritten runs;
        #    resolve duplicate SHA between push and pull_request.
        #    With no branch context at all (bare repository_dispatch) skip
        #    supersede/dedup entirely: an unfiltered listing would see runs
        #    of other branches and could cancel or skip on foreign runs.
        args.head_ref = normalize_branch(args.head_ref)
        stale = False
        if args.head_ref:
            # A run may sit in the runner queue while the branch moves on
            # (push or force-push). When this run's head is no longer the
            # branch tip, the run is stale: the current-tip run owns
            # supersede/dedup for the branch, and this run must neither
            # cancel it (the stale run would classify the current tip as
            # "rewritten history" and kill the authoritative gate) nor run
            # a gate for the abandoned content.
            try:
                tip = api(args.repo,
                          f"branches/{urllib.parse.quote(args.head_ref)}",
                          args.token)["commit"]["sha"]
                stale = tip != args.head_sha
            except Exception as e:  # noqa: BLE001 — no verdict, keep old behavior
                print(f"orchestrate: branch-tip lookup failed ({e}); "
                      f"assuming this run is current", file=sys.stderr)
            if stale:
                print(f"orchestrate: stale run: branch {args.head_ref} tip is "
                      f"{tip[:12]}, not {args.head_sha[:12]}; skipping the gate",
                      file=sys.stderr)
                out["skip"] = True
        if args.head_ref and not stale:
            head_runs = list_runs(args.repo, args.token, args.workflow_id,
                                  branch=args.head_ref)
            # github.run_id arrives as a string, the API returns int ids:
            # compare in one type, or the own-run exemption never matches
            # and the run dedups/cancels against itself.
            current_run_id = str(args.run_id) if args.run_id else ""
            for run in head_runs:
                # Same-content dedup between the push and pull_request gates of
                # the same SHA. The pull_request gate outranks the push one
                # (ci-dispatch: "PR wins"):
                #   - a live same-SHA push run is superseded (cancelled) by the
                #     PR gate instead of the PR run skipping itself;
                #   - a push run finished successfully means the coverage is
                #     already provided, so the PR run skips;
                #   - a push run finished with a failure leaves the gate empty,
                #     so the PR run proceeds to provide the signal;
                #   - the push leg defers to a live or successful pull_request
                #     run, and reruns after a failed one.
                # A *cancelled* same-SHA run must NOT lead to a skip: it produced
                # no result, and skipping on top of it would leave the gate
                # empty; there is nothing to cancel either.
                if run["head_sha"] == args.head_sha and str(run["id"]) != current_run_id:
                    if run.get("conclusion") == "cancelled":
                        print(f"orchestrate: same-SHA run {run['id']} ({run['event']}) "
                              f"is cancelled; proceeding", file=sys.stderr)
                        continue
                    alive = run["status"] in ("in_progress", "queued", "pending", "waiting")
                    finished_ok = run.get("conclusion") == "success"
                    gate_pair = (args.event, run["event"])
                    if gate_pair == ("pull_request", "push"):
                        if finished_ok:
                            print(f"orchestrate: duplicate push run {run['id']} "
                                  f"finished successfully; skip this pull_request run",
                                  file=sys.stderr)
                            out["skip"] = True
                        elif alive:
                            print(f"orchestrate: superseding the live push run {run['id']} "
                                  f"on the same SHA (PR wins)", file=sys.stderr)
                            cancel_runs(args.repo, args.token, [run["id"]], args.dry_run)
                            out["cancel_ids"].append(run["id"])
                        else:
                            print(f"orchestrate: duplicate push run {run['id']} failed; "
                                  f"this pull_request run provides the signal", file=sys.stderr)
                        continue
                    if gate_pair == ("push", "pull_request"):
                        if finished_ok or alive:
                            print(f"orchestrate: duplicate {run['event']} run {run['id']} "
                                  f"on the same SHA; skip this {args.event} run",
                                  file=sys.stderr)
                            out["skip"] = True
                        else:
                            print(f"orchestrate: duplicate pull_request run {run['id']} failed; "
                                  f"this push run provides the signal", file=sys.stderr)
                        continue
                    print(f"orchestrate: duplicate {run['event']} run {run['id']} "
                          f"on the same SHA; skip this {args.event} run",
                          file=sys.stderr)
                    out["skip"] = True
                    continue
                # Only unfinished runs can/need to be cancelled.
                if run["status"] not in ("in_progress", "queued", "pending", "waiting"):
                    continue
                # Never cancel this run itself: its own entry is listed while
                # it is in_progress, and the same-SHA dedup above passes it by
                # (id == current_run_id) straight into this branch.
                if str(run["id"]) == current_run_id:
                    continue
                # different head: superseded (ancestor) or rewritten (not ancestor)
                anc = git(["merge-base", "--is-ancestor", run["head_sha"], "HEAD"])
                # Preserve-unaffected supersede (issue #224, coarse model):
                # a superseded run stays alive when the follow-up diff touches
                # no common_prefixes file — its in-flight cells are provably
                # unaffected, and the new run only spawns the complement. Any
                # common-code change (or a rewritten-history run, or a run
                # without a base anchor) still cancels-and-reruns as before.
                if (anc.returncode == 0 and base_sha
                        and not sc.hits_common(
                            json.loads(REGISTRY.read_text(encoding="utf-8")),
                            diff_paths(run["head_sha"], args.head_sha))):
                    est = cells_for_paths(diff_paths(base_sha, run["head_sha"]))
                    out["preserved"].append(run["id"])
                    for cid in est:
                        if cid not in out["preserved_cells"]:
                            out["preserved_cells"].append(cid)
                    print(f"orchestrate: preserving {run['event']} run {run['id']} "
                          f"({run['head_sha'][:12]}): follow-up diff touches no "
                          f"common code, ~{len(est)} cells stay in flight",
                          file=sys.stderr)
                    continue
                kind = "superseded" if anc.returncode == 0 else "rewritten-history"
                print(f"orchestrate: cancelling {kind} {run['event']} run {run['id']} "
                      f"({run['head_sha'][:12]})", file=sys.stderr)
                cancel_runs(args.repo, args.token, [run["id"]], args.dry_run)
                out["cancel_ids"].append(run["id"])

        # 4. cells
        if out["skip"]:
            out["cells"] = ""
        elif base_sha:
            cells = select_cells(base_sha=base_sha)
            if out["preserved_cells"]:
                sel = [l for l in cells.splitlines() if l]
                spawn = [c for c in sel if c not in set(out["preserved_cells"])]
                print(f"orchestrate: preserve metrics: selected {len(sel)}, "
                      f"spawn {len(spawn)}, duplicated {len(sel) - len(spawn)} "
                      f"(kept in-flight runs: {len(out['preserved'])})",
                      file=sys.stderr)
                # literal [] keeps the run job skipped while the preserved
                # runs carry the coverage; an empty string would read as the
                # fatal-selection signal instead.
                out["cells"] = "\n".join(spawn) if spawn else "[]"
            else:
                out["cells"] = cells
                print(f"orchestrate: selected {len(cells.splitlines())} cells "
                      f"(diff vs {base_sha[:12]})", file=sys.stderr)
        else:
            out["cells"] = select_cells(profile="full")
            print(f"orchestrate: selected full gate ({len(out['cells'].splitlines())} cells)",
                  file=sys.stderr)
    except Exception as e:
        print(f"orchestrate: fatal: {e}", file=sys.stderr)
        # Fail-safe: report skip=false, no cancellations, and request the full
        # gate so coverage is never silently reduced.
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
