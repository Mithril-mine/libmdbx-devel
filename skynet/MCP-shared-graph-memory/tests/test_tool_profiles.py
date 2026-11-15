"""Профили инструментов: урезанный набор tools/list для не-libmdbx проектов.

Профиль (SHARED_GRAPH_MEMORY_TOOL_PROFILE) — список имён инструментов; всё
остальное не публикуется в tools/list и не вызывается. Контракт:
1) без профиля публикуются все инструменты;
2) с профилем — ровно его имена; вызов скрытого = Unknown tool;
3) опечатка в профиле или неизвестный профиль — громкая ошибка, а не
   молча урезанный набор;
4) поставляемые профили ссылаются только на существующие инструменты.
"""

import os
from unittest.mock import MagicMock

import pytest

from mcp.errors import MemoryError
from mcp.mcp_server import PROFILES_DIR, McpServer, load_tool_profile


def _names(server):
    resp = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
    return {t["name"] for t in resp["result"]["tools"]}


def test_no_profile_lists_all_tools():
    full = McpServer(MagicMock())
    assert len(_names(full)) > 100


def test_knowledge_profile_lists_only_its_tools():
    allowed = load_tool_profile("knowledge")
    server = McpServer(MagicMock(), allowed_tools=allowed)
    names = _names(server)
    assert names == allowed
    assert {"safe_store", "recall", "search", "db_set_mode"} <= names
    assert not names & {"cg_symbols", "coverage_insert", "probe_add", "branch_register"}


def test_hidden_tool_is_not_callable():
    server = McpServer(MagicMock(), allowed_tools=load_tool_profile("knowledge"))
    resp = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                          "params": {"name": "cg_symbols", "arguments": {}}})
    assert resp["error"]["message"] == "Unknown tool: cg_symbols"


def test_profile_from_path_ignores_comments(tmp_path):
    p = tmp_path / "mine.txt"
    p.write_text("# memory only\nrecall\n\nsafe_store  # write path\n")
    assert load_tool_profile(str(p)) == {"recall", "safe_store"}


def test_unknown_tool_in_profile_fails_loud():
    with pytest.raises(MemoryError) as exc:
        McpServer(MagicMock(), allowed_tools={"recall", "recal"})
    assert "recal" in str(exc.value) and "TOOL_PROFILE" in str(exc.value)


def test_unknown_profile_fails_loud():
    with pytest.raises(MemoryError) as exc:
        load_tool_profile("no-such-profile")
    assert "no-such-profile" in str(exc.value)


def test_main_applies_profile_from_env(monkeypatch, tmp_path):
    from mcp.mcp_server import main

    seen = {}

    def fake_loop(self, stdin=None, stdout=None):
        seen["tools"] = set(self.tools)

    monkeypatch.setattr(McpServer, "loop", fake_loop)
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PATH", str(tmp_path / "db.mdbx"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_TOOL_PROFILE", "knowledge")
    assert main([]) == 0
    assert seen["tools"] == load_tool_profile("knowledge")


def test_main_bad_profile_fails_before_opening_db(monkeypatch, tmp_path, capsys):
    from mcp.mcp_server import main

    db = tmp_path / "db.mdbx"
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PATH", str(db))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_TOOL_PROFILE", "no-such-profile")
    assert main([]) == 2
    assert not db.exists()
    assert "TOOL_PROFILE" in capsys.readouterr().err


@pytest.mark.parametrize("name", sorted(
    f[:-4] for f in os.listdir(PROFILES_DIR) if f.endswith(".txt")))
def test_shipped_profiles_name_existing_tools(name):
    McpServer(MagicMock(), allowed_tools=load_tool_profile(name))
