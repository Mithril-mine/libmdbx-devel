"""Тесты моста с codebase-memory-mcp (опциональный, работает без CBM)."""

import json
import os


def test_cbm_link_unlink_linked(store):
    r = store.cbm_link("fn:api-env:mdbx_env_open", "fn:env:open",
                       cbm_project="libmdbx-devel")
    assert r["cbm_node"] == "fn:env:open"
    linked = store.cbm_linked(prefix="fn:api-env")
    assert len(linked) == 1
    assert linked[0]["cbm_node"] == "fn:env:open"
    store.cbm_unlink("fn:api-env:mdbx_env_open")
    assert store.cbm_linked(prefix="fn:api-env") == []


def test_cbm_search_unavailable_without_binary(store, monkeypatch):
    """Без бинаря CBM модуль работает: возвращает unavailable, не ошибка."""
    monkeypatch.delenv("CBM_BIN", raising=False)
    # принудительно «убираем» бинарь
    monkeypatch.setattr(store, "cbm_binary", staticmethod(lambda: ""))
    r = store.cbm_search("mdbx_env_open")
    assert r["unavailable"] is True


def test_cbm_search_uses_binary_and_returns_envelope(store, tmp_path, monkeypatch):
    """С фейковым CBM-бинарем вызов проходит и отдаёт MCP-обёртку."""
    fake = tmp_path / "cbm"
    fake.write_text(
        "#!/bin/sh\n"
        "printf '{\"content\":[{\"type\":\"text\",\"text\":\"ok %s\"}],\"isError\":false}\\n' \"$3\"\n")
    fake.chmod(0o755)
    monkeypatch.delenv("CBM_BIN", raising=False)
    monkeypatch.setattr(store, "cbm_binary", staticmethod(lambda: str(fake)))
    r = store.cbm_search("mdbx_env_open", tool="search_graph")
    assert r["cbm"] is True
    assert r["tool"] == "search_graph"


def test_cbm_link_persists_after_reopen(store):
    store.cbm_link("macro:MDBX_DUPSORT", "m:dup")
    got = store.cbm_linked(prefix="macro:")
    assert got and got[0]["our_key"] == "macro:MDBX_DUPSORT"