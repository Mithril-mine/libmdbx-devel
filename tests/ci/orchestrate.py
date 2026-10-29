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
#    "cancel_ids": [ids]}
#   cells may be empty only when skip=true (duplicate superseded by another
#   run on the same SHA); otherwise empty cells is a fatal signal.
#
# --dry-run prints the plan without tagging/cancelling.

import argparse
import json
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TAG_PREFIX = "ci-ok-"


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
    for run in successful:
        tag = f"{TAG_PREFIX}{run['run_number']}"
        if dry_run:
            made.append((tag, run["head_sha"]))
            continue
        r = git(["tag", tag, run["head_sha"]])
        if r.returncode != 0:
            print(f"orchestrate: tag {tag} failed: {r.stderr.strip()}", file=sys.stderr)
            continue
        made.append((tag, run["head_sha"]))
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


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--workflow-id", required=True)
    parser.add_argument("--token", default="")
    parser.add_argument("--head-ref", required=True)
    parser.add_argument("--head-sha", required=True)
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--event", required=True)
    parser.add_argument("--run-id", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    out = {"base_sha": "", "cells": "", "skip": False, "cancel_ids": []}
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
        head_runs = list_runs(args.repo, args.token, args.workflow_id,
                              branch=args.head_ref)
        current_run_id = args.run_id
        for run in head_runs:
            # Same-content dedup: a live same-SHA run (still running or already
            # finished successfully) makes this run a duplicate — the first one
            # is authoritative, so skip. A *cancelled* same-SHA run must NOT
            # lead to a skip: it produced no result, and skipping on top of it
            # would leave the gate empty; there is nothing to cancel either.
            if run["head_sha"] == args.head_sha and run["id"] != current_run_id:
                if run.get("conclusion") == "cancelled":
                    print(f"orchestrate: same-SHA run {run['id']} ({run['event']}) "
                          f"is cancelled; proceeding", file=sys.stderr)
                    continue
                print(f"orchestrate: duplicate {run['event']} run {run['id']} "
                      f"on the same SHA; skip this {args.event} run",
                      file=sys.stderr)
                out["skip"] = True
                continue
            # Only unfinished runs can/need to be cancelled.
            if run["status"] not in ("in_progress", "queued", "pending", "waiting"):
                continue
            # different head: superseded (ancestor) or rewritten (not ancestor)
            anc = git(["merge-base", "--is-ancestor", run["head_sha"], "HEAD"])
            kind = "superseded" if anc.returncode == 0 else "rewritten-history"
            print(f"orchestrate: cancelling {kind} {run['event']} run {run['id']} "
                  f"({run['head_sha'][:12]})", file=sys.stderr)
            cancel_runs(args.repo, args.token, [run["id"]], args.dry_run)
            out["cancel_ids"].append(run["id"])

        # 4. cells
        if out["skip"]:
            out["cells"] = ""
        elif base_sha:
            r = subprocess.run(
                [sys.executable, str(REPO_ROOT / "tests/ci/select-cells.py"),
                 "--base", base_sha],
                capture_output=True, text=True, cwd=REPO_ROOT)
            if r.returncode == 0:
                out["cells"] = "\n".join(l for l in r.stdout.splitlines() if l)
                print(f"orchestrate: selected {len(out['cells'].splitlines())} cells "
                      f"(diff vs {base_sha[:12]})", file=sys.stderr)
            else:
                print(f"orchestrate: select-cells failed: {r.stderr.strip()}", file=sys.stderr)
                raise RuntimeError("select-cells --base failed")
        else:
            r = subprocess.run(
                [sys.executable, str(REPO_ROOT / "tests/ci/select-cells.py"),
                 "--profile", "full"],
                capture_output=True, text=True, cwd=REPO_ROOT)
            if r.returncode == 0:
                out["cells"] = "\n".join(l for l in r.stdout.splitlines() if l)
                print(f"orchestrate: selected full gate ({len(out['cells'].splitlines())} cells)",
                      file=sys.stderr)
            else:
                print(f"orchestrate: select-cells --profile full failed: {r.stderr.strip()}",
                      file=sys.stderr)
                raise RuntimeError("select-cells --profile full failed")
    except Exception as e:
        print(f"orchestrate: fatal: {e}", file=sys.stderr)
        # Fail-safe: report skip=false, no cancellations, and request the full
        # gate so coverage is never silently reduced.
        out = {"base_sha": "", "cells": subprocess.run(
            [sys.executable, str(REPO_ROOT / "tests/ci/select-cells.py"), "--profile", "full"],
            capture_output=True, text=True, cwd=REPO_ROOT).stdout,
            "skip": False, "cancel_ids": []}
    finally:
        cleanup_tags(args.dry_run)

    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
