"""Тесты моста с codebase-memory-mcp (опциональный, работает без CBM)."""


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
    """cbm_search вызывает CBM-подпроцесс и отдаёт MCP-обёртку.

    Mock через subprocess.run (платформонезависимо; на Windows POSIX-
    shebang-скрипты не исполняются).
    """
    import subprocess as sp
    monkeypatch.delenv("CBM_BIN", raising=False)
    monkeypatch.setattr(store, "cbm_binary", staticmethod(lambda: "fake-cbm"))
    calls = {}

    def fake_run(cmd, capture_output=True, text=True, timeout=60, check=False):
        calls["cmd"] = cmd
        out = '{"content":[{"type":"text","text":"ok fake"}],"isError":false}'
        return sp.CompletedProcess(cmd, 0, stdout=out, stderr="")

    monkeypatch.setattr(sp, "run", fake_run)
    r = store.cbm_search("mdbx_env_open", tool="search_graph")
    assert r["cbm"] is True
    assert r["tool"] == "search_graph"
    assert "search_graph" in calls["cmd"]


def test_cbm_link_persists_after_reopen(store):
    store.cbm_link("macro:MDBX_DUPSORT", "m:dup")
    got = store.cbm_linked(prefix="macro:")
    assert got and got[0]["our_key"] == "macro:MDBX_DUPSORT"