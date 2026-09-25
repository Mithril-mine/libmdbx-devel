"""Тесты MCP-сервера (JSON-RPC stdio, прямое обращение к handle)."""

import json

import pytest

from mcp_memory.errors import MemoryError
from mcp_memory.mcp_server import McpServer
from tests.conftest import seed_vocab


@pytest.fixture
def server(store):
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android"),
                       ("platform", "android-abi")])
    return McpServer(store)


def call(server, method, params=None, mid=1):
    msg = {"jsonrpc": "2.0", "id": mid, "method": method}
    if params is not None:
        msg["params"] = params
    return server.handle(msg)


def test_initialize(server):
    r = call(server, "initialize")
    assert r["result"]["serverInfo"]["name"] == "mcp-memory"
    assert r["result"]["protocolVersion"]


def test_ping(server):
    assert call(server, "ping")["result"] == {}


def test_notifications_ignored(server):
    assert call(server, "notifications/initialized") is None


def test_tools_list(server):
    r = call(server, "tools/list")
    names = {t["name"] for t in r["result"]["tools"]}
    for expected in ("safe_store", "recall", "search", "lookup", "link",
                     "unlink", "graph", "vocab_add", "vocab_find", "exists",
                     "dump_context", "restore_context", "gc", "purge", "stats",
                     "normalize_key"):
        assert expected in names


def test_unknown_method(server):
    r = call(server, "no/such")
    assert r["error"]["code"] == -32601


def test_unknown_tool(server):
    r = call(server, "tools/call", {"name": "nope", "arguments": {}})
    assert r["error"]["code"] == -32602


def test_safe_store_tool(server):
    r = call(server, "tools/call", {"name": "safe_store", "arguments": {
        "key": "bug:crypto:alignment-arm64", "type": "bug",
        "summary": "Segfault misaligned.", "importance": 0.7}})
    assert "error" not in r
    res = r["result"]["structuredContent"]
    assert res["result"] == "created"


def test_safe_store_contract_error(server):
    """Ошибка контракта должна уйти как JSON-RPC protocol error (-32000)."""
    r = call(server, "tools/call", {"name": "safe_store", "arguments": {
        "key": "bug:nosuchmod:theme", "type": "bug",
        "summary": "text", "importance": 0.5}})
    assert r["error"]["code"] == -32000
    msg = r["error"]["message"]
    assert msg.startswith("error$")
    assert "CLASS=" in msg and "ACTION=" in msg and "RETRY=" in msg


def test_recall_tool(server):
    call(server, "tools/call", {"name": "safe_store", "arguments": {
        "key": "bug:crypto:alignment", "type": "bug",
        "summary": "Some bug.", "importance": 0.7}})
    r = call(server, "tools/call", {"name": "recall", "arguments": {"pattern": "bug:"}})
    records = r["result"]["structuredContent"]["records"]
    assert len(records) == 1


def test_stats_tool(server):
    r = call(server, "tools/call", {"name": "stats", "arguments": {}})
    assert "records" in r["result"]["structuredContent"]


def test_gc_tool(server):
    r = call(server, "tools/call", {"name": "gc", "arguments": {}})
    assert r["result"]["structuredContent"]["dry_run"] is True


def _seed(server):
    call(server, "tools/call", {"name": "safe_store", "arguments": {
        "key": "bug:crypto:alignment-arm64", "type": "bug",
        "summary": "Segfault misaligned.", "importance": 0.7}})
    call(server, "tools/call", {"name": "safe_store", "arguments": {
        "key": "fact:platform:android-abi", "type": "fact",
        "summary": "Android ABIs.", "importance": 0.9}})
    call(server, "tools/call", {"name": "link", "arguments": {
        "subject_key": "bug:crypto:alignment", "predicate": "related-to",
        "object_key": "fact:platform:android-abi"}})


def test_tool_normalize_key(server):
    r = call(server, "tools/call", {"name": "normalize_key",
                                    "arguments": {"raw": "Bug:Crypto:Alignment-ARM64"}})
    assert r["result"]["structuredContent"]["canonical_key"] == "bug:crypto:alignment"


def test_tool_search(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "search",
                                    "arguments": {"query": "segfault"}})
    records = r["result"]["structuredContent"]["records"]
    assert any(x["key"] == "bug:crypto:alignment" for x in records)


def test_tool_lookup(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "lookup", "arguments": {"term": "segfault"}})
    assert r["result"]["structuredContent"]["record_keys"] == ["bug:crypto:alignment"]


def test_tool_link_unlink_graph(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "graph", "arguments": {
        "key": "bug:crypto:alignment", "depth": 1}})
    g = r["result"]["structuredContent"]
    assert len(g["edges"]) == 1
    r = call(server, "tools/call", {"name": "unlink", "arguments": {
        "subject_key": "bug:crypto:alignment", "predicate": "related-to",
        "object_key": "fact:platform:android-abi"}})
    assert r["result"]["structuredContent"]["result"] == "unlinked"


def test_tool_vocab_add_find(server):
    r = call(server, "tools/call", {"name": "vocab_add",
                                    "arguments": {"module": "custom2"}})
    assert r["result"]["structuredContent"]["result"] == "added"
    r = call(server, "tools/call", {"name": "vocab_find",
                                    "arguments": {"query": "custom"}})
    assert "custom2" in r["result"]["structuredContent"]["modules"]


def test_tool_exists(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "exists",
                                    "arguments": {"key": "bug:crypto:alignment"}})
    assert r["result"]["structuredContent"]["exists"] is True


def test_tool_context(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "dump_context", "arguments": {
        "task": "T", "keys": ["bug:crypto:alignment"]}})
    cid = r["result"]["structuredContent"]["context_id"]
    r2 = call(server, "tools/call", {"name": "restore_context",
                                     "arguments": {"context_id": cid}})
    assert r2["result"]["structuredContent"]["records"]


def test_tool_purge(server):
    _seed(server)
    r = call(server, "tools/call", {"name": "purge", "arguments": {
        "keys": ["bug:crypto:alignment"]}})
    assert r["result"]["structuredContent"]["removed"] == ["bug:crypto:alignment"]


def test_resources_list(server):
    r = call(server, "resources/list")
    assert r["result"]["resources"] == []


def test_resources_templates(server):
    r = call(server, "resources/templates/list")
    assert r["result"]["resourceTemplates"] == []


def test_logging_setlevel(server):
    r = call(server, "logging/setLevel")
    assert r["result"] == {}


def test_tools_call_generic_exception(server):
    # limit нечисловой -> int() ValueError -> JSON-RPC error
    r = call(server, "tools/call", {"name": "recall",
                                    "arguments": {"pattern": "bug:", "limit": "abc"}})
    assert r["error"]["code"] == -32000
    assert "recall" in r["error"]["message"]


def test_dispatch_unknown_tool(server):
    with pytest.raises(MemoryError):
        server._dispatch("no-such-tool", {})