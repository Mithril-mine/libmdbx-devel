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
    raw = {**col.functions, **col.types, **col.macros}
    symbols, key_map = s.canonicalize_symbols(raw)
    col.edges = [(key_map.get(c, c), n) for c, n in col.edges]
    edges = s.resolve_edges(col, symbols)
    return col, edges, symbols, key_map


def test_c_fixture_functions_and_edges():
    col, edges, symbols, _ = _scan(os.path.join(FIXTURES, "scan_fixture.c"))
    names = {v["name"] for v in col.functions.values()}
    assert {"helper_impl", "scan_fixture_entry"} <= names
    fn = col.functions["fn:scan_fixture:scan_fixture_entry"]
    assert fn["is_definition"] is True
    assert fn["l0"] == 11
    assert fn["l1"] == 24
    assert len(fn["blocks"]) >= 3  # for/if/while
    assert {e["callee"] for e in edges} == {"fn:helper_impl"}
    assert all(e["resolved"] for e in edges)


def test_c_fixture_types_and_macros():
    _clang()
    col, _, _, _ = _scan(os.path.join(FIXTURES, "scan_fixture.c"))
    assert "type:scan_fixture:fixture_record" in col.types
    entry = {"command": "", "file": os.path.join(FIXTURES, "scan_fixture.c"),
             "directory": FIXTURES}
    col2 = s.Collector(default_file=entry["file"], tu_dir=FIXTURES)
    s.scan_macros(_clang(), "fixture", entry, col2)
    macros = {k.rsplit(":", 1)[-1] for k in col2.macros}
    assert {"SCAN_FIXTURE_MAGIC", "SCAN_FIXTURE_DOUBLE"} <= macros


def test_cxx_fixture_class_methods_and_calls():
    col, edges, symbols, _ = _scan(os.path.join(FIXTURES, "scan_fixture.cxx"))
    keys = set(symbols)
    # канонизированные ключи: namespace из mangledName (fixture::Widget)
    assert "fn:fixture::Widget::Widget" in keys  # конструктор
    assert "fn:fixture::Widget::compute" in keys  # out-of-line метод
    assert "fn:fixture::Widget::helper" in keys   # inline static метод
    assert "fn:scan_fixture_cxx_entry" in keys
    # вызовы резолвятся в квалифицированные методы
    by_callee = {}
    for e in edges:
        by_callee.setdefault(e["callee"], []).append(e)
    helper_edges = by_callee.get("fn:fixture::Widget::helper", [])
    assert len(helper_edges) == 1
    assert helper_edges[0]["caller"] == "fn:fixture::Widget::compute"
    compute_edges = by_callee.get("fn:fixture::Widget::compute", [])
    assert len(compute_edges) == 1
    assert compute_edges[0]["caller"] == "fn:scan_fixture_cxx_entry"


def test_cxx_fixture_type():
    col, _, _, _ = _scan(os.path.join(FIXTURES, "scan_fixture.cxx"))
    assert "type:scan_fixture:Widget" in col.types


def test_module_of():
    assert s.module_of("src/alloy.c") == "alloy"
    assert s.module_of("src/mdbx.c++") == "mdbx"
    assert s.module_of("src/tools/copy.c") == "copy"


def test_block_id_stability_under_insertion():
    """Вставка строк ВЫШЕ функции не меняет path-based id блоков."""
    import tempfile
    base = os.path.join(FIXTURES, "block_stability_fixture.c")
    prefix = "/* %s */\n" % "x" * 40  # произвольные строки сверху
    with open(base) as f:
        code = f.read()
    # собираем блоки исходной фикстуры
    with tempfile.NamedTemporaryFile("w", suffix=".c", dir=FIXTURES,
                                     delete=False) as tmp:
        tmp.write(code)
        tmp_path = tmp.name
    col1, _, _, _ = _scan(tmp_path)
    os.unlink(tmp_path)
    # та же структура, но +40 строк выше (имитация правок до функции)
    with tempfile.NamedTemporaryFile("w", suffix=".c", dir=FIXTURES,
                                     delete=False) as tmp:
        tmp.write(prefix + code)
        tmp_path2 = tmp.name
    col2, _, _, _ = _scan(tmp_path2)
    os.unlink(tmp_path2)

    b1 = next(v["blocks"] for v in col1.functions.values())
    b2 = next(v["blocks"] for v in col2.functions.values())
    ids1 = [b["id"] for b in b1]
    ids2 = [b["id"] for b in b2]
    assert ids1 == ids2, "block-id изменился при вставке строк выше"
    # структурный формат: B, B1.1, ...
    assert ids1[0] == "B1"
    assert any("." in i for i in ids1)


def test_canonicalize_tu_static():
    """Одинаковые имя+сигнатура в разных файлах (tools) — разные функции."""
    symbols = {
        "fn:copy:signal_handler": {"kind": "function", "name": "signal_handler",
                                   "signature": "void (int)", "file": "src/tools/copy.c",
                                   "module": "copy", "l0": 1, "l1": 5,
                                   "is_definition": True, "static": True},
        "fn:stat:signal_handler": {"kind": "function", "name": "signal_handler",
                                   "signature": "void (int)", "file": "src/tools/stat.c",
                                   "module": "stat", "l0": 1, "l1": 5,
                                   "is_definition": True, "static": True},
    }
    out, mapping = s.canonicalize_symbols(symbols)
    assert set(out) == {"fn:signal_handler@copy", "fn:signal_handler@stat"}
    assert mapping["fn:copy:signal_handler"] == "fn:signal_handler@copy"


def test_canonicalize_overloads():
    """Разные сигнатуры с одним qname (C++ перегрузки) — #sig-hash."""
    symbols = {
        "fn:mdbx:error::error": {"kind": "function", "name": "error::error",
                                 "signature": "void (MDBX_error_t)", "module": "mdbx",
                                 "file": "src/mdbx.c++", "l0": 1, "l1": 2,
                                 "is_definition": True, "static": False},
        "fn:mdbx:error::error~2": {"kind": "function", "name": "error::error",
                                   "signature": "void (const error &)", "module": "mdbx",
                                   "file": "src/mdbx.c++", "l0": 4, "l1": 6,
                                   "is_definition": True, "static": False},
    }
    out, _ = s.canonicalize_symbols(symbols)
    keys = set(out)
    assert len(keys) == 2
    assert all(k.startswith("fn:error::error#") for k in keys)


def test_canonicalize_inline_dedup():
    """Inline-функция хидера, видимая в 2 TU — один символ + extra_defs."""
    symbols = {
        "fn:mdbx:__inline_mdbx_env_stat": {
            "kind": "function", "name": "__inline_mdbx_env_stat",
            "signature": "int (const MDBX_env *)", "module": "mdbx",
            "file": "src/../mdbx.h", "l0": 10, "l1": 40,
            "is_definition": True, "static": False},
        "fn:alloy:__inline_mdbx_env_stat": {
            "kind": "function", "name": "__inline_mdbx_env_stat",
            "signature": "int (const MDBX_env *)", "module": "alloy",
            "file": "src/../mdbx.h", "l0": 10, "l1": 39,
            "is_definition": True, "static": False},
    }
    out, _ = s.canonicalize_symbols(symbols)
    # дедуп: один символ, большее определение — главное
    assert "fn:__inline_mdbx_env_stat" in out
    assert out["fn:__inline_mdbx_env_stat"]["extra_defs"]


def test_canonicalize_decl_only():
    """Декларации без тела: одна запись на qname+модуль."""
    symbols = {
        "fn:mdbx:mdbx_env_open": {"kind": "function", "name": "mdbx_env_open",
                                  "signature": "int (MDBX_env *)", "module": "mdbx",
                                  "file": "src/mdbx.c++", "l0": 50, "l1": 50,
                                  "is_definition": False, "static": False},
    }
    out, _ = s.canonicalize_symbols(symbols)
    assert out["fn:mdbx_env_open"]["is_definition"] is False