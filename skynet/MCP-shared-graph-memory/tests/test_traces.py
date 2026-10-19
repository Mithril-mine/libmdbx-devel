"""Тесты наблюдаемых вызовов (runtime traces) и semantic-рёбер."""

import json


def test_trace_ingest_and_observed(store):
    store.trace_ingest("fn:a:b", "fn:c:d", count=1)
    store.trace_ingest("fn:a:b", "fn:c:d", count=2)  # инкремент
    store.trace_ingest("fn:a:b", "fn:e:f", count=1)
    recs = store.trace_observed("fn:a:b")
    by_callee = {r["callee"]: r["count"] for r in recs}
    assert by_callee == {"fn:c:d": 3, "fn:e:f": 1}
    # префикс фильтрует
    assert store.trace_observed("fn:a:") == store.trace_observed("fn:a:b")
    assert store.trace_observed("fn:nope") == []


def test_trace_path_bfs(store):
    store.trace_ingest("A", "B")
    store.trace_ingest("B", "C")
    store.trace_ingest("C", "D")
    paths = store.trace_path("A", "D", depth=5)
    assert ["A", "B", "C", "D"] in paths
    # нет пути
    assert store.trace_path("A", "ZZZ", depth=5) == []


def test_trace_compare_static_vs_observed(store):
    # static-ребро в call_edges
    store.map_put_edge("fn:lib:foo", "fn:lib:bar", kind="syntax")
    # наблюдаем только одно из двух
    store.trace_ingest("fn:lib:foo", "fn:lib:bar")
    cmp = store.trace_compare("fn:lib:foo")
    assert cmp["static"] == ["fn:lib:bar"]
    assert cmp["observed"] == ["fn:lib:bar"]
    assert cmp["only_static"] == []
    # статическое ребро, которое НЕ наблюдалось
    store.map_put_edge("fn:lib:foo", "fn:lib:unused", kind="syntax")
    cmp2 = store.trace_compare("fn:lib:foo")
    assert "fn:lib:unused" in cmp2["only_static"]


def test_map_put_edge_semantic_kind(store):
    store.map_put_edge("fn:gc-get:gc_alloc_ex@linux",
                       "fn:gc-get:gc_alloc_ex@win32", kind="semantic")
    edge = json.loads(store.map_edges_of("fn:gc-get:gc_alloc_ex@linux")[0])
    assert edge["kind"] == "semantic"
    assert edge["callee"] == "fn:gc-get:gc_alloc_ex@win32"