#!/usr/bin/env python3
"""collect_test_metrics.py — загрузка метрик тестов (ctest XML) в Store.

Читает ctest-артефакты (Testing/*/Test.xml, — или LastTest.log/ctest --output)
и обновляет скользящее окно длительностей каждого тест-кейса в таблице
`test_metrics` (Store.test_metrics_insert). Также заполняет `ci_jobs` из
CTestTestfile-структуры, если передан --jobs из workflows.

Использование:
    python3 tools/collect_test_metrics.py --xml build/Testing/20261018-0102/Test.xml
    python3 tools/collect_test_metrics.py --ctest-dir build --db ~/.local/share/shared-graph-memory/db.mdbx
    python3 tools/collect_test_metrics.py --list-tests build
"""

import argparse
import glob
import json
import os
import sys
import time
import xml.etree.ElementTree as ET

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402


def parse_ctest_xml(path: str) -> list:
    """Извлекает (name, duration_ms, status) из Test.xml ctest."""
    out = []
    tree = ET.parse(path)
    root = tree.getroot()
    for test in root.iter("Test"):
        name = test.findtext("Name") or test.findtext("FullName") or "?"
        status = test.get("Status", "passed")
        dur_ms = 0.0
        for nm in test.iter("NamedMeasurement"):
            if (nm.get("name") == "Execution Time"
                    and nm.findtext("Value")):
                try:
                    dur_ms = float(nm.findtext("Value")) * 1000.0
                except ValueError:
                    pass
        out.append({"name": name, "ms": dur_ms, "status": status})
    return out


def parse_last_test_log(path: str) -> list:
    """Fallback: LastTest.log со строками вида '1/42 Test #1: name .... Passed 1.2 sec'."""
    out = []
    try:
        with open(path, errors="replace") as fh:
            for line in fh:
                # "1/42 Test #3: some_name ............   Passed   0.12 sec"
                if line.rstrip() and "Test #" in line:
                    name = line.split("Test #", 1)[1].split(":", 1)[1]
                    if "...." in name:
                        rest = name.split("....", 1)[1]
                        name_part = name.split("....", 1)[0].strip()
                        sec = 0.0
                        tail = rest
                        if " sec" in tail:
                            num = tail.split("sec")[0].strip().rsplit(" ", 1)[-1]
                            try:
                                sec = float(num)
                            except ValueError:
                                sec = 0.0
                        status = "passed" if "Passed" in tail else "failed"
                        out.append({"name": name_part, "ms": sec * 1000.0,
                                    "status": status})
    except OSError:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml", default=None)
    ap.add_argument("--ctest-dir", default=None,
                    help="каталог сборки: ищет Testing/*/Test.xml [glob]")
    ap.add_argument("--last-log", default=None)
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--jobs", default=None,
                    help="путь к .github/workflows или каталогу workflows: "
                         "дополнительно заполняет ci_jobs (map_ci_jobs)")
    ap.add_argument("--repo", default=None,
                    help="корень репозитория (для --jobs по умолчанию: ../..)")
    ap.add_argument("--list-tests", default=None,
                    help="вывести (name, ms) из XML/LastLog без записи")
    args = ap.parse_args()

    rows = []
    if args.list_tests:
        if args.list_tests.endswith(".xml"):
            rows = parse_ctest_xml(args.list_tests)
        else:
            rows = parse_last_test_log(args.list_tests)
        for r in sorted(rows, key=lambda x: -x["ms"]):
            print("%8.1f ms  %-45s %s" % (r["ms"], r["name"], r["status"]))
        return 0
    if args.xml:
        rows = parse_ctest_xml(args.xml)
    elif args.last_log:
        rows = parse_last_test_log(args.last_log)
    elif args.ctest_dir:
        files = sorted(glob.glob(os.path.join(args.ctest_dir, "Testing", "*", "Test.xml")))
        if not files:
            last = os.path.join(args.ctest_dir, "Testing", "Temporary", "LastTest.log")
            if os.path.exists(last):
                rows = parse_last_test_log(last)
        for f in files:
            rows.extend(parse_ctest_xml(f))
    else:
        ap.error("укажите --xml | --last-log | --ctest-dir | --list-tests")

    if not rows:
        print("no tests parsed", file=sys.stderr)
        return 2

    if args.dry_run:
        print("parsed %d test rows" % len(rows))
        return 0

    store = Store(args.db)
    try:
        t0 = time.time()
        n = 0
        for r in rows:
            store.test_metrics_insert(r["name"], r["ms"],
                                      labels=[r["status"]],
                                      timeout_sec=0)
            n += 1
        dt = time.time() - t0
        print("test_metrics: %d rows in %.1fs" % (n, dt))
        if args.jobs:
            # заполняем ci_jobs из workflows (переиспользуем map_ci_jobs)
            import map_ci_jobs as mci
            wf = args.jobs
            if not os.path.isdir(wf):
                wf = os.path.dirname(wf)
            # cell-реестр: без него matrix.cell даёт пустой tests_hint и
            # может стереть ранее резолвленные отображения
            repo = args.repo or os.path.join(MODULE_DIR, "..", "..")
            cells = mci.load_cells(os.path.join(repo, "tests", "ci",
                                                "config.json"))
            jobs = []
            for wfpath in sorted(glob.glob(os.path.join(wf, "*.yml"))):
                for wname, jid, body in mci.iter_jobs(wfpath):
                    jobs.append(mci.summarize(wname, jid, body, cells=cells))
            for j in jobs:
                store.ci_jobs_insert("%s/%s" % (j["workflow"], j["job"]), j)
            print("ci_jobs: %d jobs" % len(jobs))
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())