#!/usr/bin/env python3
"""map_ci_jobs.py — парсер CI-workflows в таблицу `ci_jobs`.

Читает `.github/workflows/*.yml` (PyYAML) и `tests/ci/config.json`
(реестр cells: тест-наборы по path-зонам), формирует для каждой джобы:
  {job}: {workflow, runs_on, matrix, scope, coverage, ndk, cell,
          tests_hint, merged: bool}

Заполняется через Store.ci_jobs_insert / ci_jobs_batch.

Использование:
  python3 tools/map_ci_jobs.py [--repo <root>] [--db ...] [--dry-run]
"""

import argparse
import glob
import json
import os
import sys
import time

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))

from mcp import Store  # noqa: E402


def iter_jobs(workflow_path: str):
    """Извлекает (workflow_name, job_id, job_body) из YAML."""
    if yaml is None:
        raise SystemExit("PyYAML не установлен (pip install pyyaml)")
    with open(workflow_path) as fh:
        data = yaml.safe_load(fh) or {}
    name = data.get("name") or os.path.basename(workflow_path)
    jobs = data.get("jobs") or {}
    for jid, body in jobs.items():
        if not isinstance(body, dict):
            continue
        yield name, jid, body


def summarize(workflow_name: str, jid: str, body: dict,
              cells: dict = None) -> dict:
    """Компактное описание джобы для ci_jobs."""
    runs_on = body.get("runs-on", None)
    strategy = body.get("strategy", {}) or {}
    matrix = strategy.get("matrix", None)
    out = {
        "workflow": workflow_name,
        "job": jid,
        "runs_on": runs_on,
        "matrix": matrix if isinstance(matrix, dict) else None,
        "scope": None,
        "coverage": None,
        "ndk": None,
        "cell": None,
        "tests_hint": [],
    }
    # matrix может содержать ключи scope/coverage/ndk/cell
    if isinstance(matrix, dict):
        for k in ("scope", "coverage", "ndk", "cell"):
            v = matrix.get(k)
            if isinstance(v, list):
                out[k] = v
            elif v is not None:
                out[k] = [v]
    # enterprises: берём из инлайн-переменной / шагов (приблизительно)
    env = body.get("env") or {}
    for k in ("scope", "coverage", "ndk", "cell"):
        if out.get(k) is None and env.get(k):
            out[k] = [env[k]]
    # tests hint: если cell известен — из реестра cells (format: list[dict])
    if cells and isinstance(out.get("cell"), list):
        for c in out["cell"]:
            rec = None
            for cand in cells:
                if isinstance(cand, dict) and cand.get("id") == c:
                    rec = cand
                    break
            if rec:
                ctest = rec.get("ctest") or []
                if isinstance(ctest, str):
                    ctest = [ctest]
                out["tests_hint"].extend(ctest)
                out.setdefault("cell_id", []).append(c)
    out["tests_hint"] = sorted(set(out["tests_hint"]))[:200]
    return out


def load_cells(path: str) -> list:
    try:
        with open(path) as fh:
            data = json.load(fh)
        return data.get("cells", []) if isinstance(data, dict) else []
    except (OSError, ValueError):
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_ROOT)
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    cells = None
    cfg_path = os.path.join(args.repo, "tests", "ci", "config.json")
    cells = load_cells(cfg_path)

    wf_glob = os.path.join(args.repo, ".github", "workflows", "*.yml")
    jobs = []
    for wf in sorted(glob.glob(wf_glob)):
        for wname, jid, body in iter_jobs(wf):
            jobs.append(summarize(wname, jid, body, cells))

    print("ci_jobs: %d jobs from %s" % (len(jobs), wf_glob))
    if args.dry_run or not jobs:
        for j in jobs[:15]:
            print("  %s/%s  runs=%s matrix=%s" % (j["workflow"], j["job"],
                                                  j["runs_on"], j["matrix"]))
        return 0

    store = Store(args.db)
    try:
        t0 = time.time()
        for j in jobs:
            jkey = "%s/%s" % (j["workflow"], j["job"])
            store.ci_jobs_insert(jkey, j)
        print("ci_jobs: stored %d in %.1fs" % (len(jobs), time.time() - t0))
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())