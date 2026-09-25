"""Тесты структурного слоя (refactoring-map): symbols/call_edges/groups."""

from mcp import Store
from tests.conftest import seed_vocab


def test_map_put_and_read_symbol(store):
    body = {"kind": "function", "name": "mdbx_env_open",
            "module": "api-env", "l0": 1, "l1": 10}
    store.map_put_symbol("fn:api-env:mdbx_env_open", body)
    got = store.map_symbol("fn:api-env:mdbx_env_open")
    assert got["name"] == "mdbx_env_open"
    assert got["module"] == "api-env"
    assert store.map_symbol("missing") == {}


def test_map_symbols_prefix(store):
    for k in ("fn:gc:defrag_cycle", "fn:gc:defrag_destroy",
              "type:gc:gc_state", "macro:gc:MDBX_GC"):
        store.map_put_symbol(k, {"kind": "x", "name": k.rsplit(":", 1)[-1]})
    fns = store.map_symbols(prefix="fn:gc:")
    assert len(fns) == 2
    assert "fn:gc:defrag_cycle" in fns


def test_map_edges_dupsort(store):
    store.map_put_edge("fn:a:caller", "fn:b:callee")
    store.map_put_edge("fn:a:caller", "fn:c:other")
    # повторная вставка не должна плодить дубликаты (put_nodupe)
    store.map_put_edge("fn:a:caller", "fn:b:callee")
    edges = store.map_edges_of("fn:a:caller")
    assert sorted(edges) == sorted(["syntax\u0001fn:b:callee",
                                    "syntax\u0001fn:c:other"])
    assert store.map_edges_of("fn:zz:absent") == []


def test_map_edges_kind_semantic(store):
    store.map_put_edge("fn:a:f1", "fn:b:f2", kind="semantic")
    assert store.map_edges_of("fn:a:f1") == ["semantic\u0001fn:b:f2"]


def test_map_groups_dupsort(store):
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_cycle")
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_destroy")
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_destroy")  # дубль
    store.map_put_group("group:subsystem:env", "fn:api-env:mdbx_env_open")
    members = store.map_groups_of("group:subsystem:gc")
    assert len(members) == 2
    allpairs = store.map_group_members(prefix="group:subsystem:gc")
    assert len(allpairs) == 2


def test_map_load_batch(store):
    symbols = {
        "fn:a:foo": {"kind": "function", "name": "foo", "module": "a"},
        "type:a:bar": {"kind": "type", "name": "bar", "module": "a"},
    }
    edges = [{"caller": "fn:a:foo", "callee": "fn:a:bar",
              "kind": "syntax", "resolved": True}]
    groups = {"group:subsystem:a": {"fn:a:foo", "type:a:bar"}}
    loaded = store.map_load_batch(symbols, edges, groups)
    assert loaded == {"symbols": 2, "edges": 1, "groups": 2}
    assert store.map_symbols() == ["fn:a:foo", "type:a:bar"]
    assert store.map_edges_of("fn:a:foo") == ["syntax\u0001fn:a:bar"]
    assert store.map_groups_of("group:subsystem:a") == ["fn:a:foo",
                                                        "type:a:bar"]


def test_map_load_batch_preserves_curated(store):
    """Батч загрузки структурного слоя не должен трогать курируемый слой."""
    seed_vocab(store, [("meta", "test")])
    r = store.safe_store("fact:meta:test", "fact", "слой изолирован", 0.5)
    store.map_load_batch({"fn:a:foo": {"name": "foo", "module": "a"}}, [], {})
    assert store.exists(r["key"])


def test_map_symbol_json_roundtrip(store):
    body = {"kind": "function", "name": "mdbx_env_open",
            "signature": "int (MDBX_env *, const char *)",
            "file": "src/api-env.c", "module": "api-env",
            "l0": 414, "l1": 524,
            "blocks": [{"id": "B1", "l0": 414, "l1": 524, "kind": "compoundstmt"}]}
    store.map_put_symbol("fn:api-env:mdbx_env_open", body)
    got = store.map_symbol("fn:api-env:mdbx_env_open")
    assert got == body  # полный round-trip без потерь