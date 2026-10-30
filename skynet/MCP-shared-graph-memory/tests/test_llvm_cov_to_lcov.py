"""Тесты llvm-cov → lcov конвертера (tools/llvm_cov_to_lcov.py)."""

import json
import os
import subprocess
import sys

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools")


def _run(json_doc, root=""):
    p = os.path.join(TOOLS, "llvm_cov_to_lcov.py")
    src = "/tmp/llvmcov_test.json" if os.path.exists("/tmp") else "llvmcov_test.json"
    with open(src, "w") as fh:
        json.dump(json_doc, fh)
    r = subprocess.run([sys.executable, p, "--json", src, "--root", root],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_segments_basic_two_files():
    """Два файла, регионы с count>0/0 → только покрытые строки."""
    doc = {"data": [{"files": [
        {"filename": "/repo/src/a.c",
         "segments": [[1, 1, 1, True, True, False], [3, 1, 0, True, False, False]]},
        {"filename": "/repo/src/b.c",
         "segments": [[5, 1, 7, True, True, False]]},
    ]}]}
    out = _run(doc, "/repo")
    assert "SF:src/a.c" in out
    assert "DA:1,1" in out, "первая строка региона с count=1 — покрыта"
    assert "DA:2,1" in out, "строка внутри региона с count>0 до следующего сегмента — покрыта"
    assert "DA:3," not in out, "регион с count=0 — не покрыта"
    assert "SF:src/b.c" in out and "DA:5,1" in out


def test_empty_and_uncovered_files_skipped():
    doc = {"data": [{"files": [
        {"filename": "/repo/src/none.c", "segments": [[1, 1, 0, True, True, False]]},
        {"filename": "/repo/src/empty.c", "segments": []},
    ]}]}
    out = _run(doc, "/repo")
    assert "none.c" not in out and "empty.c" not in out


def test_windows_paths_normalized():
    doc = {"data": [{"files": [
        {"filename": "D:\\repo\\src\\windows-import.c",
         "segments": [[10, 1, 2, True, True, False]]},
    ]}]}
    out = _run(doc, "D:\\repo")
    assert "SF:src/windows-import.c" in out
    assert "DA:10,1" in out