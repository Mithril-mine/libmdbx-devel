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