"""Тесты scan_uncovered.py — острова кода вне union-AST."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "tools"))
from scan_uncovered import (covered_ranges, find_islands, _merge_spans,
                            _code_like, _classify_all, REPO_ROOT)  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tests", "fixtures")


def _tmp_file(content):
    tmp = tempfile.NamedTemporaryFile("w", suffix=".c", delete=False)
    tmp.write(content)
    tmp.close()
    return tmp.name


def test_code_like_classification():
    assert _code_like("int x = 1;")
    assert not _code_like("")
    assert not _code_like("   ")
    assert not _code_like("// comment")
    assert not _code_like("/* block */")
    assert not _code_like("* continuation")
    assert not _code_like("#define X 1")
    assert not _code_like("#ifdef FOO")


def test_merge_spans():
    spans = [(1, 5), (3, 8), (10, 12), (13, 15)]
    assert _merge_spans(spans) == [(1, 8), (10, 15)]


def test_find_islands_basic():
    code = (
        "int a(void) {\n"       # 1 covered
        "  return 1;\n"         # 2 covered
        "}\n"                   # 3 covered
        "\n"                    # 4 blank
        "int orphan;\n"         # 5 island
        "int b(void) { return 2; }\n"  # 6 island
    )
    path = _tmp_file(code)
    try:
        islands = find_islands(path, [(1, 3)])
        assert islands == [(5, 6)], islands
    finally:
        os.unlink(path)


def test_find_islands_ignores_comments_and_pp():
    code = (
        "#ifdef NEVER\n"       # 1 pp
        "int dead;\n"          # 2 island
        "// note\n"            # 3 comment
        "int more_dead;\n"     # 4 island (contiguous with 2 via comment)
        "#endif\n"             # 5 pp
    )
    path = _tmp_file(code)
    try:
        islands = find_islands(path, [])
        # комментарий между кодовыми строками разделяет острова на 2
        assert islands == [(2, 2), (4, 4)], islands
    finally:
        os.unlink(path)


def test_covered_ranges_with_implementations():
    symbols = {
        "fn:lck_seize": {
            "kind": "function",
            "implementations": [
                {"file": "src/lck-posix.c", "l0": 10, "l1": 20},
                {"file": "src/lck-windows.c", "l0": 30, "l1": 40},
            ],
        },
        "type:x": {"kind": "type", "file": "src/api-env.c", "l0": 5, "l1": 5},
        "fn:no_body": {"kind": "function", "file": "src/x.c", "l0": 0, "l1": 0},
    }
    cov = covered_ranges(symbols)
    assert cov["src/lck-posix.c"] == [(10, 20)]
    assert cov["src/lck-windows.c"] == [(30, 40)]
    assert cov["src/api-env.c"] == [(5, 5)]
    assert "src/x.c" not in cov


def test_classify_all_by_regions():
    regions = [
        {"id": "region:osal:1", "file": "src/osal.c", "l0": 1, "l1": 50,
         "cond": "__APPLE__", "cls": "platform"},
        {"id": "region:osal:2", "file": "src/osal.c", "l0": 10, "l1": 20,
         "cond": "MDBX_ENABLE_XX", "cls": "option"},
    ]
    islands = [
        {"file": "src/osal.c", "l0": 15, "l1": 16},
        {"file": "src/osal.c", "l0": 30, "l1": 31},
        {"file": "src/other.c", "l0": 1, "l1": 2},
    ]
    out = _classify_all(islands, regions)
    assert out[0]["region"] == "region:osal:2"   # самый внутренний
    assert out[0]["cls"] == "option"
    assert out[1]["region"] == "region:osal:1"
    assert out[1]["cls"] == "platform"
    assert "region" not in out[2]                # без региона


def test_scan_regions_fixture_islands():
    """Сканер на реальной фикстуре регионов: острова внутри неактивных веток."""
    import scan_symbols as s
    fixture = os.path.join(FIXTURES, "scan_regions_fixture.h")
    regs = []
    with open(fixture) as f:
        from scan_regions import RegionBuilder
        regs = RegionBuilder(fixture).parse(f.read().splitlines())
    rel = os.path.relpath(fixture, REPO_ROOT)
    syms = {
        "fn:fixture:win_only": {"kind": "function", "file": rel, "l0": 6,
                                "l1": 6, "is_definition": True},
    }
    cov = covered_ranges(syms)
    islands = find_islands(fixture, cov.get(rel, []))
    assert islands, "в фикстуре должен быть остров кода вне покрытия"