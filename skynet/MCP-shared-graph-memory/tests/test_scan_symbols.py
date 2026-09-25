"""Тесты пилотного сканера карты исходников (scan_symbols.py).

Требуют `clang` в PATH; при отсутствии — pytest.skip.
"""

import json
import os
import shutil
import subprocess
import sys

import pytest

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools")
sys.path.insert(0, TOOLS)
import scan_symbols as s  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tests", "fixtures")


def _clang():
    path = shutil.which("clang")
    if not path:
        pytest.skip("clang not in PATH")
    return path


def _scan(filepath):
    f = os.path.abspath(filepath)
    cmd = [_clang(), "-Xclang", "-ast-dump=json",
           "-Xclang", "-detailed-preprocessing-record", "-fsyntax-only", f]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr[:800]
    ast = json.loads(proc.stdout)
    col = s.Collector(default_file=f, tu_dir=os.path.dirname(f))
    s.walk(ast, col)
    s.finalize_blocks(col)
    edges = s.resolve_edges(col)
    return col, edges


def test_c_fixture_functions_and_edges():
    col, edges = _scan(os.path.join(FIXTURES, "scan_fixture.c"))
    names = {v["name"] for v in col.functions.values()}
    assert {"helper_impl", "scan_fixture_entry"} <= names
    fn = col.functions["fn:scan_fixture:scan_fixture_entry"]
    assert fn["is_definition"] is True
    assert fn["l0"] == 11
    assert fn["l1"] == 24
    assert len(fn["blocks"]) >= 3  # for/if/while
    assert {e["callee"] for e in edges} == {"fn:scan_fixture:helper_impl"}
    assert all(e["resolved"] for e in edges)


def test_c_fixture_types_and_macros():
    _clang()
    col, _ = _scan(os.path.join(FIXTURES, "scan_fixture.c"))
    assert "type:scan_fixture:fixture_record" in col.types
    entry = {"command": "", "file": os.path.join(FIXTURES, "scan_fixture.c"),
             "directory": FIXTURES}
    col2 = s.Collector(default_file=entry["file"], tu_dir=FIXTURES)
    s.scan_macros(_clang(), "fixture", entry, col2)
    macros = {k.rsplit(":", 1)[-1] for k in col2.macros}
    assert {"SCAN_FIXTURE_MAGIC", "SCAN_FIXTURE_DOUBLE"} <= macros


def test_cxx_fixture_class_methods_and_calls():
    col, edges = _scan(os.path.join(FIXTURES, "scan_fixture.cxx"))
    keys = set(col.functions)
    assert "fn:scan_fixture:Widget::Widget" in keys  # конструктор
    assert "fn:scan_fixture:Widget::compute" in keys  # out-of-line метод
    assert "fn:scan_fixture:Widget::helper" in keys   # inline static метод
    assert "fn:scan_fixture:scan_fixture_cxx_entry" in keys
    # вызовы резолвятся в квалифицированные методы
    by_callee = {}
    for e in edges:
        by_callee.setdefault(e["callee"], []).append(e)
    helper_edges = by_callee.get("fn:scan_fixture:Widget::helper", [])
    assert len(helper_edges) == 1
    assert helper_edges[0]["caller"] == "fn:scan_fixture:Widget::compute"
    compute_edges = by_callee.get("fn:scan_fixture:Widget::compute", [])
    assert len(compute_edges) == 1
    assert compute_edges[0]["caller"] == "fn:scan_fixture:scan_fixture_cxx_entry"


def test_cxx_fixture_type():
    col, _ = _scan(os.path.join(FIXTURES, "scan_fixture.cxx"))
    assert "type:scan_fixture:Widget" in col.types


def test_module_of():
    assert s.module_of("src/alloy.c") == "alloy"
    assert s.module_of("src/mdbx.c++") == "mdbx"
    assert s.module_of("src/tools/copy.c") == "copy"