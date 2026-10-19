"""Тесты слоя тестов/CI: test_metrics / coverage / ci_jobs + поколение карты."""

from tests.conftest import seed_vocab


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


def test_map_load_batch_increments_generation(store):
    """map_load_batch(replace=True) инкрементирует поколение карты (canary.y)."""
    gen0 = store.map_canary_get().get("y", 0)
    symbols = {"fn:core:m1": {"kind": "function", "module": "core"}}
    store.map_load_batch(symbols, [], {}, replace=True)
    gen1 = store.map_canary_get().get("y", 0)
    assert gen1 == gen0 + 1
    store.map_load_batch(symbols, [], {}, replace=True)
    gen2 = store.map_canary_get().get("y", 0)
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
    shutil.copy("tests/fixtures/workflow-fixture.yml", f)
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
    assert "map_generation" in v["data"]
    assert "records" in v["data"]
    store.safe_store("fact:meta:ver", "fact", "v", 0.1)
    v2 = store.version_info()
    assert v2["data"]["records"] >= 1
