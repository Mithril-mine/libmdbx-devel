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


def test_trace_path_lazy_no_full_adj(store):
    """B2: ленивый BFS корректен и не тянет недостижимые ветви."""
    # цепочка src→...→dst плюс «тяжёлая» ветвь, недостижимая из src
    store.trace_ingest("src", "m1")
    store.trace_ingest("m1", "m2")
    store.trace_ingest("m2", "dst")
    store.trace_ingest("heavy", "x1")
    store.trace_ingest("heavy", "x2")
    store.trace_ingest("x1", "x3")
    paths = store.trace_path("src", "dst", depth=6)
    assert paths == [["src", "m1", "m2", "dst"]]
    # heavy-узлы не появляются ни в одном пути из src
    flat = {n for p in paths for n in p}
    assert "heavy" not in flat and "x1" not in flat


def test_trace_compare_static_vs_observed(store):
    # static-ребро в call_edges
    store.cg_put_edge("fn:lib:foo", "fn:lib:bar", kind="syntax")
    # наблюдаем только одно из двух
    store.trace_ingest("fn:lib:foo", "fn:lib:bar")
    cmp = store.trace_compare("fn:lib:foo")
    assert cmp["static"] == ["fn:lib:bar"]
    assert cmp["observed"] == ["fn:lib:bar"]
    assert cmp["only_static"] == []
    # статическое ребро, которое НЕ наблюдалось
    store.cg_put_edge("fn:lib:foo", "fn:lib:unused", kind="syntax")
    cmp2 = store.trace_compare("fn:lib:foo")
    assert "fn:lib:unused" in cmp2["only_static"]


def test_map_put_edge_semantic_kind(store):
    store.cg_put_edge("fn:gc-get:gc_alloc_ex@linux",
                       "fn:gc-get:gc_alloc_ex@win32", kind="semantic")
    edge = json.loads(store.cg_edges_of("fn:gc-get:gc_alloc_ex@linux")[0])
    assert edge["kind"] == "semantic"
    assert edge["callee"] == "fn:gc-get:gc_alloc_ex@win32"


def test_trace_ingest_label_and_layer(store):
    """label сохраняется в агрегате; слои изолированы (C-5/C-6)."""
    store.trace_ingest("fn:a", "fn:b", count=2, label="ut-run",
                       layer="0.15.base")
    store.trace_ingest("fn:a", "fn:b", count=3, label="probe-bus",
                       layer="0.15.feat.9")
    base = store.trace_observed("fn:a", layer="0.15.base")
    feat = store.trace_observed("fn:a", layer="0.15.feat.9")
    assert len(base) == 1 and base[0]["count"] == 2
    assert base[0]["label"] == "ut-run"
    assert len(feat) == 1 and feat[0]["count"] == 3
    assert feat[0]["label"] == "probe-bus"
    # повторный ingest в base: count растёт, label прежний сохраняется,
    # новый label перезаписывает
    store.trace_ingest("fn:a", "fn:b", count=1, layer="0.15.base")
    r = store.trace_observed("fn:a", layer="0.15.base")[0]
    assert r["count"] == 3 and r["label"] == "ut-run"
    store.trace_ingest("fn:a", "fn:b", count=1, label="new", layer="0.15.base")
    r = store.trace_observed("fn:a", layer="0.15.base")[0]
    assert r["count"] == 4 and r["label"] == "new"


def test_trace_rev_no_duplicates(store):
    """call_traces_rev без дублей при повторном ingest (C-6)."""
    from mcp import libmdbx as mdbx
    store.trace_ingest("fn:x", "fn:y", layer="0.15.base")
    store.trace_ingest("fn:x", "fn:y", layer="0.15.base")
    store.trace_ingest("fn:x", "fn:y", layer="0.15.base")
    with store.env.begin(readonly=True) as txn:
        dbi = store.dbi(txn, "call_traces_rev")
        with txn.cursor(dbi) as cur:
            rc, _, _ = cur.get(mdbx.CURSOR_SET_KEY, b"fn:y")
            n = 0
            while rc == mdbx.RC_SUCCESS:
                n += 1
                rc, _, _ = cur.get(mdbx.CURSOR_NEXT_DUP)
    assert n == 1, "rev-ребро должно быть одно (nodupe)"


def test_trace_path_layer_isolated(store):
    """trace_path ходит только по рёбрам своего слоя."""
    store.trace_ingest("A", "B", layer="0.15.base")
    store.trace_ingest("B", "C", layer="0.15.base")
    store.trace_ingest("C", "D", layer="0.15.feat.1")  # другой слой
    paths = store.trace_path("A", "C", layer="0.15.base")
    assert ["A", "B", "C"] in paths
    # D недостижим из base-слоя
    assert store.trace_path("A", "D", layer="0.15.base") == []
    assert store.trace_path("C", "D", layer="0.15.feat.1") == [["C", "D"]]


def test_trace_ingest_batch(store):
    """Батчевый ingest: одна txn, накопление счётчиков, label."""
    r = store.trace_ingest_batch([
        {"caller": "fn:m1", "callee": "fn:m2", "count": 3, "label": "ci"},
        {"caller": "fn:m1", "callee": "fn:m2", "count": 2},
        {"caller": "fn:m1", "callee": "fn:m3"},
        {"caller": "", "callee": "fn:skip"},       # бракованная — пропуск
        {"caller": "fn:skip2", "callee": ""},
    ], layer="0.15.base")
    assert r["ingested"] == 3
    obs = store.trace_observed("fn:m1", layer="0.15.base")
    by = {o["callee"]: o for o in obs}
    assert by["fn:m2"]["count"] == 5
    assert by["fn:m2"]["label"] == "ci"
    assert by["fn:m3"]["count"] == 1


def test_collect_traces_parser(tmp_path):
    """collect_traces.parse_dump: text, json, jsonl."""
    import sys
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "tools"))
    from collect_traces import parse_dump
    # текстовый
    t = tmp_path / "t.txt"
    t.write_text("# comment\nfn:a fn:b 5 ci-run\nfn:c fn:d\n\nfn:e fn:f 2\n")
    pairs = parse_dump(str(t))
    assert len(pairs) == 3
    assert pairs[0] == {"caller": "fn:a", "callee": "fn:b", "count": 5,
                        "label": "ci-run"}
    assert pairs[1] == {"caller": "fn:c", "callee": "fn:d"}
    # json-массив
    j = tmp_path / "t.json"
    j.write_text('[{"caller":"x","callee":"y","count":7}]')
    assert parse_dump(str(j)) == [{"caller": "x", "callee": "y", "count": 7}]
    # jsonl
    jl = tmp_path / "t.jsonl"
    jl.write_text('{"caller":"p","callee":"q"}\n{"caller":"r","callee":"s","count":2}\n')
    assert len(parse_dump(str(jl))) == 2
