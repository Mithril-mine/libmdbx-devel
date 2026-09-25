"""Интеграционные тесты MCP-цикла (loop/main/default_db_path)."""

import io
import json
import os

from mcp.mcp_server import McpServer, _default_db_path


def test_loop_initialize_and_tools(store):
    server = McpServer(store)
    inp = io.BytesIO(
        b'{"jsonrpc":"2.0","id":1,"method":"initialize"}\n'
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n'
        b'\n'  # пустая строка пропускается
        b'not-json\n'  # мусор пропускается
        b'{"jsonrpc":"2.0","id":3,"method":"notifications/initialized"}\n'
    )
    out = io.BytesIO()
    server.loop(stdin=inp, stdout=out)
    lines = out.getvalue().decode().strip().splitlines()
    assert len(lines) == 2  # initialize + tools/list (notification без ответа)
    r1 = json.loads(lines[0])
    assert r1["id"] == 1 and r1["result"]["serverInfo"]["name"] == "shared-graph-memory.mdbx"
    r2 = json.loads(lines[1])
    assert r2["id"] == 2 and r2["result"]["tools"]


def test_loop_error_response(store):
    server = McpServer(store)
    inp = io.BytesIO(b'{"jsonrpc":"2.0","id":9,"method":"nope"}\n')
    out = io.BytesIO()
    server.loop(stdin=inp, stdout=out)
    r = json.loads(out.getvalue().decode())
    assert r["error"]["code"] == -32601


def test_default_db_path_env(monkeypatch):
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PATH", "/tmp/x.mdbx")
    assert _default_db_path() == "/tmp/x.mdbx"


def test_default_db_path_fallback(monkeypatch):
    monkeypatch.delenv("SHARED_GRAPH_MEMORY_PATH", raising=False)
    p = _default_db_path()
    assert p.endswith("db.mdbx")
    assert "shared-graph-memory" in p


def test_main_function(monkeypatch, tmp_path, store):
    from mcp.mcp_server import main

    called = {}

    def fake_loop(self, stdin=None, stdout=None):
        called["loop"] = True

    monkeypatch.setattr(McpServer, "loop", fake_loop)
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PATH", str(tmp_path / "main.mdbx"))
    rc = main([])
    assert rc == 0
    assert called.get("loop") is True