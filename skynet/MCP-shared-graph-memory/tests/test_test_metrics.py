"""Тесты слоя тестов/CI: test_metrics / coverage / ci_jobs + поколение карты."""

import os

from tests.conftest import seed_vocab

# абсолютный путь к fixtures — pytest в CI может запускаться из другого CWD
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def test_test_metrics_insert_window(store):
    """test_metrics_insert хранит скользящее окно с min/max/p90 и счётчиком флаков."""
    for i, ms in enumerate([100.0, 90.0, 110.0, 95.0], start=1):
        store.test_metrics_insert("Test/some_case", ms, labels=["ut.api"])
    m = store.test_metrics_get("Test/some_case")
    assert m["runs"] == 4
    assert m["min_ms"] == 90.0
    assert m["max_ms"] == 110.0
    assert 90.0 <= m["p90_ms"] <= 110.0
    assert m["labels"] == ["ut.api"]
    # внезапно длинный прогон → флак
    store.test_metrics_insert("Test/some_case", 5000.0)
    m2 = store.test_metrics_get("Test/some_case")
    assert m2["flaky_count"] >= 1


def test_test_metrics_all_prefix(store):
    store.test_metrics_insert("ut:extra_details", 123.0)
    store.test_metrics_insert("ut:other", 1.0)
    all_rows = store.test_metrics_all(prefix="ut:")
    assert {t["test"] for t in all_rows} == {"ut:extra_details", "ut:other"}
    subset = store.test_metrics_all(prefix="ut:extra")
    assert [t["test"] for t in subset] == ["ut:extra_details"]


def test_coverage_insert_get(store):
    store.coverage_insert("ut/api/cursor-api", ["fn:api-cursor:mdbx_cursor_get",
                                                "fn:api-cursor:mdbx_cursor_put"],
                          pct=84.0)
    c = store.coverage_get("ut/api/cursor-api")
    assert len(c["symbols"]) == 2
    assert c["pct"] == 84.0
    cov = store.coverage_all()
    assert len(cov) == 1


def test_ci_jobs_insert_all(store):
    store.ci_jobs_insert("linux/gcc-asan", {"matrix": "gcc@13", "tests": "ut"})
    store.ci_jobs_insert("windows/msvc", {"matrix": "msvc-2022"})
    jobs = store.ci_jobs_all(prefix="linux/")
    assert [j["job"] for j in jobs] == ["linux/gcc-asan"]
    assert jobs[0]["body"]["matrix"] == "gcc@13"


def test_cg_load_batch_increments_generation(store):
    """cg_load_batch(replace=True) инкрементирует поколение карты (canary.y)."""
    gen0 = store.cg_canary_get().get("y", 0)
    symbols = {"fn:core:m1": {"kind": "function", "module": "core"}}
    store.cg_load_batch(symbols, [], {}, replace=True)
    gen1 = store.cg_canary_get().get("y", 0)
    assert gen1 == gen0 + 1
    store.cg_load_batch(symbols, [], {}, replace=True)
    gen2 = store.cg_canary_get().get("y", 0)
    assert gen2 == gen1 + 1


def test_links_in_reflects_purge(store):
    """purge уменьшает денормализованный счётчик links_in у связанных узлов."""
    from mcp.store import _pack_u64, _unpack_u64
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android")])
    store.safe_store("bug:crypto:alignment", "bug", "bug one", 0.5)
    store.safe_store("fact:platform:android", "fact", "fact one", 0.5)
    fact_id = store.get_record("fact:platform:android")["_id"]
    store.link("bug:crypto:alignment", "caused-by", "fact:platform:android")
    store.purge(["bug:crypto:alignment"])
    with store.env.begin(readonly=True) as txn:
        _, v = txn.get(store.dbi(txn, "links_in"), _pack_u64(fact_id))
        assert v is None or _unpack_u64(v) == 0


def test_coverage_covered_by_reverse_index(store):
    """После coverage_insert по символу находятся защищающие его тесты."""
    store.coverage_insert("ut/api/cursor", ["fn:api-cursor:mdbx_cursor_get",
                                            "fn:api-cursor:mdbx_cursor_put"],
                          pct=80.0)
    store.coverage_insert("ut/api/cursors2", ["fn:api-cursor:mdbx_cursor_put"])
    r = store.coverage_symbols("fn:api-cursor:mdbx_cursor_get")
    assert len(r) == 1 and r[0]["tests"] == ["ut/api/cursor"]
    r2 = store.coverage_symbols("fn:api-cursor:mdbx_cursor_put")
    assert {t for x in r2 for t in x["tests"]} == {"ut/api/cursor",
                                                   "ut/api/cursors2"}
    # удаление теста чистит обратный индекс
    store.coverage_remove("ut/api/cursors2")
    r3 = store.coverage_symbols("fn:api-cursor:mdbx_cursor_put")
    assert {t for x in r3 for t in x["tests"]} == {"ut/api/cursor"}


def test_coverage_batch(store):
    """coverage_batch фиксирует несколько тестов в одной транзакции."""
    store.coverage_batch({
        "ut/t1": {"symbols": ["fn:a:b"], "pct": 10.0},
        "ut/t2": {"symbols": ["fn:c:d"], "pct": 20.0},
    })
    assert store.coverage_get("ut/t1")["symbols"] == ["fn:a:b"]
    assert store.coverage_get("ut/t2")["pct"] == 20.0


def test_ci_jobs_get_and_by_test(store):
    store.ci_jobs_insert("linux/gcc", {"workflow": "linux", "job": "gcc",
                                       "tests_hint": ["ut/api/cursor", "ut/txn"]})
    store.ci_jobs_insert("windows/msvc", {"workflow": "windows", "job": "msvc"})
    assert store.ci_jobs_get("linux/gcc")["job"] == "gcc"
    jobs = store.ci_jobs_by_test("ut/api/cursor")
    assert jobs == ["linux/gcc"]
    jobs2 = store.ci_jobs_by_test("missing/test")
    assert jobs2 == []


def test_map_ci_jobs_parser(tmp_path):
    """map_ci_jobs: парсинг workflow YAML в компактные описания джоб."""
    import shutil
    import tools.map_ci_jobs as mci
    f = str(tmp_path / "wf.yml")
    shutil.copy(os.path.join(FIXTURES, "workflow-fixture.yml"), f)
    jobs = list(mci.iter_jobs(f))
    assert len(jobs) == 2
    s = mci.summarize(jobs[0][0], jobs[0][1], jobs[0][2])
    assert s["workflow"] == "ci-fixture"
    assert s["runs_on"] == "ubuntu-24.04"
    assert "coverage" in s["matrix"]
    assert "cc" in s["matrix"]
    s2 = mci.summarize(jobs[1][0], jobs[1][1], jobs[1][2])
    assert s2["matrix"] is None


def test_version_info_fields(store):
    """version_info отдаёт версии модуля/libmdbx/схемы/данных."""
    v = store.version_info()
    assert v["module_version"] == "0.2.0"
    assert v["schema_version"] == "2"
    assert v["libmdbx_version"]
    assert "cg_generation" in v["data"]
    assert "records" in v["data"]
    store.safe_store("fact:meta:ver", "fact", "v", 0.1)
    v2 = store.version_info()
    assert v2["data"]["records"] >= 1


def test_map_ci_jobs_cells_resolve_tests(tmp_path):
    """map_ci_jobs: cells list[dict] c id+ctest резолвится в tests_hint."""
    import shutil
    import tools.map_ci_jobs as mci
    wf = str(tmp_path / "wf.yml")
    shutil.copy(os.path.join(FIXTURES, "workflow-fixture.yml"), wf)
    cfg = tmp_path / "config.json"
    cfg.write_text('{"cells": [{"id": "linux-gcc-cursor", '
                   '"ctest": ["ut/api/cursor", "ut/cursor"]}]}')
    cells = mci.load_cells(str(cfg))
    jobs = list(mci.iter_jobs(wf))
    s = mci.summarize(jobs[0][0], jobs[0][1], jobs[0][2], cells=cells)
    # у fixture workflow нет cell в matrix — tests_hint остаётся пустым
    assert s["tests_hint"] == []
    # принудительно зададим cell и проверим резолв
    s2 = dict(jobs[0][2])
    s2["strategy"] = {"matrix": {"cell": ["linux-gcc-cursor"]}}
    s3 = mci.summarize(jobs[0][0], jobs[0][1], s2, cells=cells)
    assert sorted(s3["tests_hint"]) == ["ut/api/cursor", "ut/cursor"]


def test_coverage_blocks_insert_get(store):
    """Блочная детализация покрытия: хранение и чтение по символу."""
    n = store.coverage_blocks_insert("ut/gc", [
        {"symbol": "fn:gc_alloc_ex", "block_id": "B1", "l0": 1, "l1": 2,
         "covered": True},
        {"symbol": "fn:gc_alloc_ex", "block_id": "B2", "l0": 5, "l1": 8,
         "covered": False},
    ])
    assert n == 1
    blocks = store.coverage_blocks_get("ut/gc", "fn:gc_alloc_ex")
    assert len(blocks) == 2
    by_id = {b["block_id"]: b["covered"] for b in blocks}
    assert by_id == {"B1": True, "B2": False}


def test_coverage_blocks_batch(store, monkeypatch):
    """coverage_blocks_batch (A3): одна txn на прогон, семантика insert."""
    orig = store._begin_write
    calls = {"n": 0}

    def spy():
        calls["n"] += 1
        return orig()

    monkeypatch.setattr(store, "_begin_write", spy)
    res = store.coverage_blocks_batch({
        "ut/gc": [{"symbol": "fn:gc_alloc_ex", "block_id": "B1",
                   "covered": True}],
        "ut/api": [{"symbol": "fn:mdbx_get", "block_id": "B1",
                    "covered": True},
                   {"symbol": "fn:mdbx_get", "block_id": "B2",
                    "covered": False}],
    })
    monkeypatch.undo()
    assert res == {"tests": 2, "symbols": 2}
    assert calls["n"] == 1, "батч = одна write-txn, %d" % calls["n"]
    # содержимое идентично поштучной записи
    b1 = store.coverage_blocks_get("ut/gc", "fn:gc_alloc_ex")
    b2 = store.coverage_blocks_get("ut/api", "fn:mdbx_get")
    assert [b["block_id"] for b in b1] == ["B1"]
    assert {b["block_id"]: b["covered"] for b in b2} == \
        {"B1": True, "B2": False}


def test_test_metrics_batch(store, monkeypatch):
    """test_metrics_batch (A3): одна txn на ctest-прогон, окна как у insert."""
    orig = store._begin_write
    calls = {"n": 0}

    def spy():
        calls["n"] += 1
        return orig()

    monkeypatch.setattr(store, "_begin_write", spy)
    res = store.test_metrics_batch({
        "ut/a": {"duration_ms": 100.0, "labels": ["passed"],
                 "timeout_sec": 0},
        "ut/b": {"duration_ms": 250.0, "labels": ["failed"],
                 "timeout_sec": 0},
    })
    monkeypatch.undo()
    assert res == {"tests": 2}
    assert calls["n"] == 1, "батч = одна write-txn, %d" % calls["n"]
    ma = store.test_metrics_get("ut/a")
    mb = store.test_metrics_get("ut/b")
    assert ma["runs"] == 1 and ma["last_ms"] == 100.0
    assert ma["labels"] == ["passed"]
    assert mb["last_ms"] == 250.0 and mb["labels"] == ["failed"]


def test_coverage_gaps_finds_uncovered_symbols(store):
    """coverage_gaps('gc') находит символы gc без покрытия."""
    store.cg_put_symbol("fn:gc_alloc_ex", {"module": "gc-get", "kind": "function"})
    store.cg_put_symbol("fn:gc_update", {"module": "gc-put", "kind": "function"})
    store.cg_put_symbol("fn:spill_cycle", {"module": "spill", "kind": "function"})
    # покрыт только gc_alloc_ex
    store.coverage_insert("ut/gc", ["fn:gc_alloc_ex"])
    gaps = store.coverage_gaps(module="gc")
    syms = {g["symbol"] for g in gaps["uncovered_symbols"]}
    assert "fn:gc_update" in syms
    assert "fn:gc_alloc_ex" not in syms
    assert gaps["uncovered_symbols_count"] >= 1


def test_coverage_layer_isolation(store):
    """Покрытие в feature-слое изолировано от базового (C-5)."""
    store.coverage_insert("ut/api", ["fn:base_sym"], pct=50.0, layer="0.15.base")
    store.coverage_insert("ut/api", ["fn:feat_sym"], pct=60.0,
                          layer="0.15.feat.3")
    base = store.coverage_get("ut/api", layer="0.15.base")
    feat = store.coverage_get("ut/api", layer="0.15.feat.3")
    assert base["symbols"] == ["fn:base_sym"]
    assert feat["symbols"] == ["fn:feat_sym"]
    # coverage_all base не видит feature-тест
    tests_base = {c["test"] for c in store.coverage_all(layer="0.15.base")}
    assert "ut/api" in tests_base
    # covered_by изолирован
    r = store.coverage_symbols("fn:feat_sym", layer="0.15.feat.3")
    assert r and r[0]["tests"] == ["ut/api"]
    r0 = store.coverage_symbols("fn:feat_sym", layer="0.15.base")
    assert r0 == []


def test_coverage_remove_cleans_blocks(store):
    """coverage_remove чистит coverage_blocks теста (C-3)."""
    store.coverage_insert("ut/gc", ["fn:gc_alloc"], layer="0.15.base")
    store.coverage_blocks_insert("ut/gc", [
        {"symbol": "fn:gc_alloc", "block_id": "B1", "l0": 1, "l1": 2,
         "covered": True}], layer="0.15.base")
    assert store.coverage_blocks_get("ut/gc", "fn:gc_alloc",
                                     layer="0.15.base")
    store.coverage_remove("ut/gc", layer="0.15.base")
    assert store.coverage_blocks_get("ut/gc", "fn:gc_alloc",
                                     layer="0.15.base") == []
    assert store.coverage_get("ut/gc", layer="0.15.base") == {}


def test_coverage_batch_recreates_blocks_state(store):
    """coverage_batch: смена набора символов чистит stale блоки (C-3)."""
    store.coverage_batch({"ut/x": {"symbols": ["fn:a", "fn:b"]}},
                         layer="0.15.base")
    store.coverage_blocks_insert("ut/x", [
        {"symbol": "fn:b", "block_id": "B9", "l0": 1, "l1": 1,
         "covered": True}], layer="0.15.base")
    # повторный batch БЕЗ fn:b — stale блоки fn:b удалены
    store.coverage_batch({"ut/x": {"symbols": ["fn:a"]}}, layer="0.15.base")
    assert store.coverage_blocks_get("ut/x", "fn:b", layer="0.15.base") == []


def test_coverage_summary_union(store):
    """coverage_summary: union-агрегация по модулям (A4)."""
    store.cg_put_symbol("fn:gc_alloc_ex", {"module": "gc-get",
                                            "kind": "function",
                                            "l0": 1, "l1": 10,
                                            "blocks": [{"id": "B1"}, {"id": "B2"}]},
                         layer="0.15.base")
    store.cg_put_symbol("fn:gc_put", {"module": "gc-put", "kind": "function",
                                       "l0": 1, "l1": 5,
                                       "blocks": [{"id": "B1"}]},
                         layer="0.15.base")
    store.coverage_insert("ut1", ["fn:gc_alloc_ex"], layer="0.15.base")
    store.coverage_blocks_insert("ut1", [
        {"symbol": "fn:gc_alloc_ex", "block_id": "B1", "l0": 1, "l1": 5,
         "covered": True},
        {"symbol": "fn:gc_alloc_ex", "block_id": "B2", "l0": 6, "l1": 10,
         "covered": False}], layer="0.15.base")
    s = store.coverage_summary(layer="0.15.base")
    assert s["modules"]["gc-get"]["symbols_covered"] == 1
    assert s["modules"]["gc-get"]["symbols_total"] == 1
    assert s["modules"]["gc-get"]["blocks_covered"] == 1
    assert s["modules"]["gc-get"]["blocks_total"] == 2
    assert s["modules"]["gc-get"]["pct"] == 50.0
    assert s["modules"]["gc-put"]["symbols_covered"] == 0
    # фильтр по модулю
    s2 = store.coverage_summary(module="gc-get", layer="0.15.base")
    assert "gc-put" not in s2["modules"]
    assert s2["total"]["symbols_total"] == 1
