"""Тесты #if-дерева (scan_regions.py): парсинг, классификация, привязка.

Не требует clang — чистый текстовый анализ директив.
Проверяет вложенность #if/#elif/#else/#endif (вплоть до 4 уровней),
классификацию условий и привязку символов к регионам.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "tools"))
from scan_regions import RegionBuilder  # noqa: E402
from scan_symbols import link_symbols_to_regions, REPO_ROOT  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tests", "fixtures")
# относительный путь фикстуры от REPO_ROOT (зависит от расположения модуля:
# в рабочей копии это .../skynet/MCP-shared-graph-memory, в установке иначе)
FIXTURE_REL = os.path.relpath(os.path.join(FIXTURES, "scan_regions_fixture.h"),
                              REPO_ROOT)


def _parse(fixture):
    with open(os.path.join(FIXTURES, fixture)) as f:
        return RegionBuilder(os.path.join(FIXTURES, fixture)).parse(
            f.read().splitlines())


def test_region_count_and_kinds():
    regs = _parse("scan_regions_fixture.h")
    kinds = {r["kind"] for r in regs}
    assert kinds == {"if", "elif", "else", "ifndef"}
    assert sum(1 for r in regs if r["kind"] == "if") == 7
    assert sum(1 for r in regs if r["kind"] == "ifndef") == 1
    assert sum(1 for r in regs if r["kind"] == "elif") == 1
    assert sum(1 for r in regs if r["kind"] == "else") == 5


def test_classification():
    regs = _parse("scan_regions_fixture.h")
    by_cond = {r["cond"]: r for r in regs}
    assert by_cond["IS_WINDOWS"]["cls"] == "platform"
    assert by_cond["defined(__linux__)"]["cls"] == "platform"
    assert by_cond["MDBX_ENABLE_PROFGC"]["cls"] == "option"
    assert by_cond["MDBX_DEBUG"]["cls"] == "option"


def test_nesting_and_parents():
    regs = _parse("scan_regions_fixture.h")
    ids = {r["id"] for r in regs}
    by_cond = {r["cond"]: r for r in regs}

    profgc = by_cond["MDBX_ENABLE_PROFGC"]
    debug = by_cond["MDBX_DEBUG"]
    assert debug["parent"] == profgc["block_id"]
    assert debug["depth"] == profgc["depth"] + 1

    # guard закрывает всё
    guard = by_cond["!defined(SCAN_REGIONS_GUARD)"]
    for r in regs:
        if r["depth"] == 1 and r["block_id"] != guard["block_id"]:
            assert r["parent"] == guard["block_id"]

    # все block_id и parent ссылаются на существующие регионы
    assert all(r["block_id"] in ids for r in regs)
    assert all(r["parent"] is None or r["parent"] in ids for r in regs)


def test_deep_nesting_4_levels():
    regs = _parse("scan_regions_fixture.h")
    by_cond = {r["cond"]: r for r in regs}
    gnuc = by_cond["defined(__GNUC__)"]
    gcc8 = by_cond["__GNUC__ >= 8"]
    x86 = by_cond["defined(__x86_64__)"]
    assert x86["depth"] == gnuc["depth"] + 2
    assert gcc8["parent"] == gnuc["block_id"]
    assert x86["parent"] == gcc8["block_id"]
    assert max(r["depth"] for r in regs) >= 3


def test_region_ranges_no_overlap_of_siblings():
    """Ветки одного блока не должны перекрываться по l0..l1."""
    regs = _parse("scan_regions_fixture.h")
    from collections import defaultdict
    by_block = defaultdict(list)
    for r in regs:
        by_block[r["block_id"]].append(r)
    for bid, branches in by_block.items():
        sorted_b = sorted(branches, key=lambda r: r["l0"])
        for a, b in zip(sorted_b, sorted_b[1:]):
            # следующая ветка начинается после конца предыдущей
            assert b["l0"] >= a["l1"], "%s: %s(%d) overlaps %s(%d)" % (
                bid, a["kind"], a["l0"], b["kind"], b["l0"])


def test_location_detection():
    regs = _parse("scan_regions_fixture.h")
    by_cond = {r["cond"]: r for r in regs}
    assert by_cond["IS_WINDOWS"]["location"] == "definition-gating"
    assert by_cond["MDBX_PNL_ASCENDING"]["location"] == "inner-function"
    assert by_cond["MDBX_DEBUG"]["location"] == "inner-function"
    assert by_cond["defined(__GNUC__)"]["location"] == "definition-gating"


def test_link_symbols_to_regions():
    regs = _parse("scan_regions_fixture.h")
    syms = {
        "fn:fixture:win_only": {"name": "win_only", "file": FIXTURE_REL,
                                "l0": 6, "l1": 6},
        "fn:fixture:always_here": {"name": "always_here",
                                   "file": FIXTURE_REL,
                                   "l0": 23, "l1": 29},
        "fn:fixture:deep_nested_x86": {"name": "deep_nested_x86",
                                       "file": FIXTURE_REL,
                                       "l0": 34, "l1": 34},
    }
    linked = link_symbols_to_regions(syms, regs)

    win = linked["fn:fixture:win_only"]
    assert "regions" in win
    assert win["regions"][0].startswith("region:")
    # самая внутренняя покрывающая ветка — IS_WINDOWS
    by_id = {r["id"]: r for r in regs}
    inner = by_id[win["regions"][-1]]
    assert inner["cond"] == "IS_WINDOWS"

    always = linked["fn:fixture:always_here"]
    # функция внутри include-guard: покрыта только внешним guard-регионом
    assert "regions" in always
    by_id = {r["id"]: r for r in regs}
    inner_guard = by_id[always["regions"][-1]]
    assert inner_guard["cond"] == "!defined(SCAN_REGIONS_GUARD)"

    deep = linked["fn:fixture:deep_nested_x86"]
    assert "regions" in deep
    # цепочка: defined(__GNUC__) → __GNUC__ >= 8 → defined(__x86_64__)
    chain = [by_id[rid] for rid in deep["regions"]]
    conds = [c["cond"] for c in chain]
    assert conds[-3:] == ["defined(__GNUC__)", "__GNUC__ >= 8",
                          "defined(__x86_64__)"]


def test_annotate_configs():
    from scan_symbols import annotate_configs
    regs = _parse("scan_regions_fixture.h")
    syms = {
        "fn:fixture:win_only": {"name": "win_only", "file": FIXTURE_REL,
                                "l0": 6, "l1": 6},
        "fn:fixture:always_here": {"name": "always_here",
                                   "file": FIXTURE_REL,
                                   "l0": 23, "l1": 29},
    }
    linked = link_symbols_to_regions(syms, regs)
    annotated = annotate_configs(linked, regs)
    win_cfg = annotated["fn:fixture:win_only"]["configs"]
    assert "win32" in win_cfg
    # всегда видимый (внутри только guard) — опции не выведены, но не 'all'
    always_cfg = annotated["fn:fixture:always_here"]["configs"]
    assert isinstance(always_cfg, list)