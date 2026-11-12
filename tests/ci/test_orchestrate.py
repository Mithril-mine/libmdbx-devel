# Unit tests for tests/ci/orchestrate.py pure helpers (no network).
#
# Run: python3 -m pytest tests/ci/test_orchestrate.py -q
# or: python3 tests/ci/test_orchestrate.py

import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orchestrate  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent


def git(*a):
    return subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True)


def head_shas():
    """Current tip and its parent (fresh per call: the branch advances)."""
    return (git("rev-parse", "HEAD").stdout.strip(),
            git("rev-parse", "HEAD~1").stdout.strip())


@pytest.fixture(autouse=True)
def clean_tags():
    for t in git("tag", "-l", "ci-ok-*").stdout.split():
        git("tag", "-d", t)
    yield
    for t in git("tag", "-l", "ci-ok-*").stdout.split():
        git("tag", "-d", t)


def test_nearest_ancestor_prefers_closest():
    git("tag", "ci-ok-far", "HEAD~3")
    git("tag", "ci-ok-near", "HEAD~1")
    sha, dist = orchestrate.nearest_ancestor_sha()
    assert sha is not None
    # HEAD~1 must be chosen over HEAD~3 (dist could be >1 with merge parents,
    # but must be strictly less than the far one).
    far_dist = int(git("rev-list", "--count", f"{git('rev-parse','HEAD~3').stdout.strip()}..HEAD").stdout)
    assert dist < far_dist
    assert git("rev-parse", "HEAD~1").stdout.strip() == sha


def test_nearest_ancestor_ignores_non_ancestors():
    # A tag on a commit that is not an ancestor of HEAD must be ignored.
    # We use an unrelated ref if available; otherwise create one via a sibling
    # branch tip. Simpler: a tag pointing at a commit in another branch.
    branches = git("for-each-ref", "refs/remotes/origin", "--format=%(refname)").stdout.split()
    other = None
    for br in branches:
        sha = git("rev-parse", br).stdout.strip()
        if sha and git("merge-base", "--is-ancestor", sha, "HEAD").returncode != 0:
            other = sha
            break
    if other is None:
        pytest.skip("no non-ancestor commit available")
    git("tag", "ci-ok-other", other)
    # If only a non-ancestor tag exists, nothing is selected.
    # (With no ancestor tags at all, nearest returns (None, None).)
    # To keep the test independent of other ci-ok-* tags (none in repo), assert
    # that the non-ancestor alone is not chosen when an ancestor exists.
    git("tag", "ci-ok-anc", "HEAD~1")
    sha, _ = orchestrate.nearest_ancestor_sha()
    assert sha == git("rev-parse", "HEAD~1").stdout.strip()


def test_classification_kind():
    # rewritten-history vs superseded classification is done inline in main();
    # here we at least assert the git primitive used for it behaves as expected.
    assert git("merge-base", "--is-ancestor", "HEAD~1", "HEAD").returncode == 0
    assert git("merge-base", "--is-ancestor", "HEAD", "HEAD~1").returncode != 0


# ---------------------------------------------------------------------------
# Supersede/dedup planning (fake Actions API; real git for classification).
# ---------------------------------------------------------------------------

def staged_run(rid, sha, status="in_progress", conclusion=None, event="push"):
    return {"id": rid, "head_sha": sha, "status": status,
            "conclusion": conclusion, "event": event, "run_number": rid}


class FakeRuns:
    """Stands in for orchestrate.list_runs/cancel_runs.

    staged: branch name -> list of run dicts; the Actions API `branch=`
    filter expects the bare branch name, so a query with a `refs/heads/`
    prefix finds nothing staged — exactly like the real service.
    """

    def __init__(self, staged=None):
        self.staged = staged or {}
        self.queries = []
        self.cancelled = []

    def list_runs(self, repo, token, workflow_id, branch=None, event=None,
                  status=None, per_page=100):
        self.queries.append(branch)
        return list(self.staged.get(branch, []))

    def cancel_runs(self, repo, token, run_ids, dry_run):
        self.cancelled.extend(run_ids)
        return list(run_ids)


def run_main(monkeypatch, capsys, fake, head_ref, head_sha, run_id="999",
             base_ref="origin/master", event="push", cells="cell-a\ncell-b"):
    monkeypatch.setattr(orchestrate, "list_runs", fake.list_runs)
    monkeypatch.setattr(orchestrate, "cancel_runs", fake.cancel_runs)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: cells)
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", head_ref, "--head-sha", head_sha,
        "--base-ref", base_ref, "--event", event, "--run-id", run_id])
    assert rc == 0
    return json.loads(capsys.readouterr().out)


def test_push_prefixed_head_ref_normalizes_and_supersedes(monkeypatch, capsys):
    """Push events pass refs/heads/<branch>; the API filter needs the bare
    name, so the orchestrator must normalize (else no supersede on push)."""
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, old)]})
    out = run_main(monkeypatch, capsys, fake, "refs/heads/feature/x", head)
    assert fake.queries, "head branch must be queried"
    assert not [q for q in fake.queries if q and q.startswith("refs/heads/")]
    assert "feature/x" in fake.queries
    assert out["cancel_ids"] == [101]
    assert out["skip"] is False


def test_empty_head_ref_disables_supersede_and_dedup(monkeypatch, capsys):
    """repository_dispatch has no branch context: the orchestrator must not
    touch runs of other branches and must not treat them as duplicates."""
    head, old = head_shas()
    cross = [staged_run(201, old), staged_run(202, head)]
    fake = FakeRuns({"": cross})
    out = run_main(monkeypatch, capsys, fake, "", head)
    assert fake.cancelled == []
    assert out["cancel_ids"] == []
    assert out["skip"] is False


def test_bare_head_ref_supersedes_ancestor_run(monkeypatch, capsys):
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, old)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == [101]
    assert out["skip"] is False


def test_same_sha_duplicate_skips(monkeypatch, capsys):
    head, _ = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, head)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["skip"] is True
    assert out["cancel_ids"] == []
    assert out["cells"] == ""


def test_rewritten_history_run_is_cancelled(monkeypatch, capsys):
    head, _ = head_shas()
    other = git("rev-parse", "stable").stdout.strip()
    if not other or git("merge-base", "--is-ancestor", other, "HEAD").returncode == 0:
        pytest.skip("no non-ancestor commit available")
    fake = FakeRuns({"feature/x": [staged_run(303, other)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == [303]
    assert out["skip"] is False


def test_own_run_is_never_a_cancel_candidate(monkeypatch, capsys):
    """The run's own entry appears in the branch listing while it is
    in_progress; it must never fall into the cancel path (a live
    self-cancel race seen on CI: the run killed itself mid-resolve).
    The int-vs-string id comparison must not break the exemption."""
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(501, old),
                                   staged_run(999, head, status="in_progress")]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert fake.cancelled == [501]
    assert out["cancel_ids"] == [501]
    assert out["skip"] is False


def test_plain_push_does_not_skip_on_own_entry(monkeypatch, capsys):
    """A push with no other run on the branch must NOT be deduplicated
    against its own listing entry: with the int/string id mismatch the run
    saw itself as a foreign duplicate and silently produced an empty gate."""
    head, _ = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(999, head, status="in_progress")]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert out["skip"] is False
    assert fake.cancelled == []
    assert out["cells"] != ""


# ---------------------------------------------------------------------------
# Preserve-unaffected supersede (issue #224, coarse conservative model).
# ---------------------------------------------------------------------------

def make_commit(parent, edits):
    """Throwaway commit on top of `parent` (unreachable from any branch).

    edits: {path: bytes}. Built via plumbing so tests control the diff
    without touching branches or the index.
    """
    def write_blob(data):
        r = subprocess.run(["git", "-C", str(REPO), "hash-object", "-w", "--stdin"],
                           input=data, capture_output=True)
        assert r.returncode == 0, r.stderr
        return r.stdout.decode().strip()

    def mktree(entries):
        lines = [f"{m} {t} {s}\t{n}" for n, (m, t, s) in entries.items()]
        r = subprocess.run(["git", "-C", str(REPO), "mktree"],
                           input="\n".join(lines) + ("\n" if lines else ""),
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return r.stdout.strip()

    def update_tree(tree, parts, blob):
        entries = {}
        for ln in git("ls-tree", tree).stdout.splitlines():
            meta, name = ln.split("\t", 1)
            m, t, s = meta.split()
            entries[name] = (m, t, s)
        name = parts[0]
        if len(parts) == 1:
            entries[name] = ("100644", "blob", blob)
        else:
            sub = entries.get(name)
            sub_tree = sub[2] if sub else mktree({})
            entries[name] = ("040000", "tree", update_tree(sub_tree, parts[1:], blob))
        return mktree(entries)

    tree = git("rev-parse", f"{parent}^{{tree}}").stdout.strip()
    for path, data in edits.items():
        tree = update_tree(tree, path.split("/"), write_blob(data))
    r = git("commit-tree", tree, "-p", parent, "-m", "ci-test: throwaway supersede commit")
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def test_docs_only_followup_push_preserves_inprogress_run(monkeypatch, capsys):
    """A follow-up commit touching no common code must keep the in-flight
    run alive (its cells are provably unaffected) and spawn only the
    complement of the preserved selection (issue #224)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head                                   # the in-flight run's SHA
    new = make_commit(head, {"docs/supersede-probe.md": b"probe note\n"})
    git("tag", "ci-ok-77", base)
    fake = FakeRuns({"feature/x": [staged_run(401, old)]})
    new_sel = ["cell-a", "cell-b", "cell-c"]
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="\n".join(new_sel))
    assert out["preserved"] == [401]
    assert 401 not in out["cancel_ids"]
    preserved_cells = set(out["preserved_cells"])
    assert preserved_cells, "the estimator must classify the old selection"
    spawn = set(out["cells"].splitlines()) if out["cells"] not in ("", "[]") else set()
    assert spawn == set(new_sel) - preserved_cells          # complement math
    assert spawn | preserved_cells >= set(new_sel)          # coverage invariant


def test_common_code_followup_push_cancels_all(monkeypatch, capsys):
    """A follow-up commit touching common code invalidates the in-flight
    cells: cancel-and-rerun as before (regression guard)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head
    new = make_commit(head, {"src/probe-supersede.h": b"/* probe */\n"})
    git("tag", "ci-ok-78", base)
    fake = FakeRuns({"feature/x": [staged_run(402, old)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="cell-a\ncell-b")
    assert out["cancel_ids"] == [402]
    assert out["preserved"] == []
    assert set(out["cells"].splitlines()) == {"cell-a", "cell-b"}


def test_preserved_only_run_emits_empty_matrix(monkeypatch, capsys):
    """When everything selected is already in flight, the cells output must
    be the literal [] (run job skipped), never an empty string (which the
    workflow treats as a fatal selection signal)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head
    new = make_commit(head, {"docs/supersede-probe.md": b"probe note\n"})
    git("tag", "ci-ok-79", base)
    est = orchestrate.cells_for_paths(
        [p for p in git("diff", "--name-only", f"{base}...{old}").stdout.splitlines() if p])
    fake = FakeRuns({"feature/x": [staged_run(403, old)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="\n".join(est))
    assert out["preserved"] == [403]
    assert out["cells"] == "[]"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
