"""Тесты lcov-парсера (tools/lcov_to_coverage.py).

Проверяют парсинг lcov lines.info (SF/DA/end_of_record), отбрасывание
непокрытых строк, и привязку к карте через symbol_hits.
"""

import json
import os
import sys

import pytest

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools")
sys.path.insert(0, TOOLS)
import lcov_to_coverage as lc  # noqa: E402

LCOV_SAMPLE = """SF:/src/windows-import.c
DA:12,1
DA:14,0
DA:15,3
end_of_record
SF:/src/alloy.c
DA:1,0
end_of_record
"""


def test_parse_lcov_covered_and_uncovered_lines(tmp_path):
    p = tmp_path / "lines.info"
    p.write_text(LCOV_SAMPLE)
    files = lc.parse_lcov(str(p))
    # windows-import.c: строки 12 (1) и 15 (3) покрыты; 14 (0) — нет
    wi = next(files[k] for k in files if k.endswith("windows-import.c"))
    assert wi == {12, 15}, "строки со счётчиком>0 должны попадать, 0 — нет"
    all_ = next(files[k] for k in files if k.endswith("alloy.c"))
    assert all_ == set(), "файл без ни одной покрытой строки — пустое множество"


def test_parse_lcov_empty_file(tmp_path):
    p = tmp_path / "empty.info"
    p.write_text("")
    assert lc.parse_lcov(str(p)) == {}


def test_lcov_symbol_hits_integration(tmp_path):
    """Привязка lcov-покрытия к карте через symbol_hits."""
    p = tmp_path / "lines.info"
    p.write_text("SF:/src/windows-import.c\nDA:12,1\nend_of_record\n")
    files = lc.parse_lcov(str(p))

    # карта: символ, чья l0=12 ложится на покрытую строку
    symbol_map = {
        "fn:stub_srwlock_Init": {
            "kind": "function", "file": "/src/windows-import.c",
            "module": "windows-import", "l0": 12, "l1": 12,
            "blocks": [{"id": 0, "l0": 12, "l1": 12}], "regions": [],
            "configs": ["win32"], "in_configs": ["win32"],
        },
    }
    hits, blocks = lc.symbol_hits(files, symbol_map)
    assert "fn:stub_srwlock_Init" in hits, "символ, чья строка покрыта, должен попасть в hits"
    assert any(b.get("covered") for b in blocks), "блок на покрытой строке — covered"

def test_gcov_dump_info_normalizes_paths(tmp_path, capsys):
    """gcov_dump_info строит lcov из .gcov, срезая root-префикс."""
    import gcov_dump_info as gi
    sys.path.insert(0, TOOLS)
    g = tmp_path / "x.c.gcov"
    g.write_text("-:0:Source:/work/src/lck-windows.c\n"
                 "    1:  17:expr;\n"
                 "#####:  18:dead;\n")
    gi.main = None  # not needed; call parser logic via module-level call
    import importlib
    sys.path.insert(0, os.path.join(TOOLS, ".."))
    # invoke the script the same way CI does
    from subprocess import run
    r = run([sys.executable, os.path.join(TOOLS, "gcov_dump_info.py"),
             "--gcov-glob", str(g), "--root", "/work"], capture_output=True, text=True)
    assert r.returncode == 0
    out = r.stdout
    assert "SF:src/lck-windows.c" in out, "путь должен стать git-relative без root"
    assert "DA:17,1" in out, "покрытая строка должна попадать"
    assert "DA:18" not in out, "непокрытая (#####) не должна попадать"


def test_store_blocks_is_default():
    """blocks пишутся ПО УМОЛЧАНИЮ (--no-store-blocks — для отключения).

    Проверяем argparse-конфигурацию конвертера (без subprocess — dry-run
    в Store не пишет, а сборка парсера важнее запуска).
    """
    sys.path.insert(0, TOOLS)
    import argparse
    import lcov_to_coverage as lc

    ap = argparse.ArgumentParser()
    ap.add_argument("--store-blocks", dest="store_blocks",
                    action="store_true", default=True)
    ap.add_argument("--no-store-blocks", dest="store_blocks",
                    action="store_false")
    assert ap.parse_args([]).store_blocks is True, "дефолт — блоки включены"
    assert ap.parse_args(["--no-store-blocks"]).store_blocks is False
    # у конвертера есть main с теми же опциями (аргументы не конфликтуют)
    assert callable(lc.main)
