"""12 сценариев использования MCP-модуля (shared-graph-memory.mdbx).

Каждый сценарий — связный поток вызовов tools/call через McpServer.handle
(JSON-RPC 2.0), как это делает агент: от словаря и нормализации до чтения,
связей, gc/purge. Словарь сеется через сам MCP (vocab_add), чтобы поток был
полностью end-to-end.
"""

import pytest

from mcp import libmdbx as mdbx
from mcp.mcp_server import McpServer


@pytest.fixture
def mcp(store):
    return McpServer(store)


def call(mcp, method, params=None, mid=1):
    msg = {"jsonrpc": "2.0", "id": mid, "method": method}
    if params is not None:
        msg["params"] = params
    return mcp.handle(msg)


def tool(mcp, name, **args):
    return call(mcp, "tools/call", {"name": name, "arguments": args})


def ok(mcp, name, **args):
    """Успешный вызов инструмента → structuredContent (или AssertionError)."""
    r = tool(mcp, name, **args)
    assert "error" not in r, "неожиданная ошибка MCP: %s" % r["error"]["message"]
    return r["result"]["structuredContent"]


def seed_bug_and_fact(mcp):
    ok(mcp, "vocab_add", module="crypto", topic="alignment")
    ok(mcp, "vocab_add", module="platform", topic="android")
    ok(mcp, "safe_store", key="bug:crypto:alignment", type="bug",
       summary="AES-CBC arm64 buffer misaligned NEON crash.", importance=0.7)
    ok(mcp, "safe_store", key="fact:platform:android", type="fact",
       summary="Android NDK ABIs arm64 x86 release builds.", importance=0.9)


# --- С1. Запись архитектурного решения ------------------------------------------
def test_s1_decision_record_full_flow(mcp):
    """Агент фиксирует решение: словарь → канонизация → запись → проверка."""
    ok(mcp, "vocab_add", module="build", topic="release-cadence")

    r = ok(mcp, "normalize_key", raw="decision:build:release cadence")
    key = r["canonical_key"]
    assert key == "decision:build:release-cadence"

    r = ok(mcp, "safe_store", key=key, type="decision",
           summary="Release cadence: two weeks, freeze on Friday.", importance=0.9)
    assert r["result"] == "created"
    assert r["key"] == key

    assert ok(mcp, "exists", key=key)["exists"] is True

    recs = ok(mcp, "recall", pattern="decision:build:*")["records"]
    assert any(x["key"] == key and x["type"] == "decision" for x in recs)


# --- С2. Дедупликация: конфликт ближнего дубликата -------------------------------
def test_s2_duplicate_conflict(mcp):
    """Два ключа с почти одинаковым текстом — вторая запись не создаётся."""
    ok(mcp, "vocab_add", module="crypto", topic="alignment")
    ok(mcp, "vocab_add", module="crypto", topic="align")

    text = "Segfault AES-CBC arm64 buffer misaligned NEON align fix."
    r = ok(mcp, "safe_store", key="bug:crypto:alignment", type="bug",
           summary=text, importance=0.7)
    assert r["result"] == "created"

    r = ok(mcp, "safe_store", key="bug:crypto:align", type="bug",
           summary=text + ".", importance=0.7)
    assert r["result"] == "conflict"
    assert r["key"] == "bug:crypto:alignment"  # указан существующий канонический ключ

    recs = ok(mcp, "recall", pattern="bug:crypto:*")["records"]
    assert len(recs) == 1  # дубль не размножил хранилище


# --- С3. Обновление существующей записи (merge + история) ------------------------
def test_s3_update_existing_record(mcp, store):
    """Повторный safe_store того же ключа обновляет запись и пишет историю."""
    ok(mcp, "vocab_add", module="platform", topic="android")
    r = ok(mcp, "safe_store", key="fact:platform:android", type="fact",
           summary="NDK r26, ABIs arm64/x86.", importance=0.6)
    assert r["result"] == "created"

    r = ok(mcp, "safe_store", key="fact:platform:android", type="fact",
           summary="NDK r27, ABIs arm64/x86/riscv64.", importance=0.8)
    assert r["result"] == "merged"

    recs = ok(mcp, "recall", pattern="fact:platform:android")["records"]
    assert recs and recs[0]["summary"] == "NDK r27, ABIs arm64/x86/riscv64."
    assert recs[0]["importance"] == 0.8

    # история сохранила прежнюю версию
    with store.env.begin(readonly=True) as txn:
        with txn.cursor(store.dbi(txn, "history")) as cur:
            rc, k, v = cur.get(mdbx.CURSOR_FIRST)
            found_old = False
            while rc == mdbx.RC_SUCCESS:
                if b"_history:fact:platform:android" in k and b"NDK r26" in v:
                    found_old = True
                    break
                rc, k, v = cur.get(mdbx.CURSOR_NEXT)
    assert found_old


# --- С4. Терм-поиск: пересечение постинг-списков ---------------------------------
def test_s4_search_intersection(mcp):
    """search по общим термам возвращает обе записи, по уточняющим — одну."""
    seed_bug_and_fact(mcp)

    recs = ok(mcp, "search", query="arm64", limit=10)["records"]
    assert len(recs) == 2

    recs = ok(mcp, "search", query="arm64 android", limit=10)["records"]
    assert [x["key"] for x in recs] == ["fact:platform:android"]

    keys = ok(mcp, "lookup", term="android")["record_keys"]
    assert set(keys) == {"fact:platform:android"}


# --- С5. Префиксный recall по типу/модулю с ранжированием -------------------------
def test_s5_prefix_recall_ranking(mcp):
    """recall('bug:*') отдаёт только баги, отсортированные по важности."""
    ok(mcp, "vocab_add", module="crypto", topic="alignment")
    ok(mcp, "vocab_add", module="crypto", topic="align")
    ok(mcp, "vocab_add", module="build", topic="release")
    ok(mcp, "safe_store", key="bug:crypto:alignment", type="bug",
       summary="Low-priority cosmetic issue in logging.", importance=0.3)
    ok(mcp, "safe_store", key="bug:crypto:align", type="bug",
       summary="Critical memory corruption on aligned stores.", importance=0.7)
    ok(mcp, "safe_store", key="proc:build:release", type="proc",
       summary="Release process uses two-week cadence.", importance=0.6)

    keys = [x["key"] for x in ok(mcp, "recall", pattern="bug:*")["records"]]
    assert set(keys) == {"bug:crypto:alignment", "bug:crypto:align"}
    assert keys[0] == "bug:crypto:align"  # выше важность → выше score

    keys = [x["key"] for x in ok(mcp, "recall", pattern="proc:build:*")["records"]]
    assert keys == ["proc:build:release"]


# --- С6. Связывание записей и обход графа -----------------------------------------
def test_s6_link_and_graph(mcp):
    """link создаёт прямую и обратную связь; graph видит обе стороны."""
    seed_bug_and_fact(mcp)

    r = ok(mcp, "link", subject_key="bug:crypto:alignment",
           predicate="related-to", object_key="fact:platform:android")
    assert r["result"] == "linked"
    # повторная попытка — уже существует
    assert ok(mcp, "link", subject_key="bug:crypto:alignment",
              predicate="related-to", object_key="fact:platform:android")["result"] == "exists"

    g = ok(mcp, "graph", key="bug:crypto:alignment", depth=1)
    assert set(g["nodes"]) == {"bug:crypto:alignment", "fact:platform:android"}
    assert any(e["subject"] == "bug:crypto:alignment" and e["object"] == "fact:platform:android"
               for e in g["edges"])

    # обход от объекта показывает обратное ребро
    g = ok(mcp, "graph", key="fact:platform:android", depth=1)
    assert any(e["subject"] == "bug:crypto:alignment" and e["object"] == "fact:platform:android"
               for e in g["edges"])


# --- С7. Разрыв связи -------------------------------------------------------------
def test_s7_unlink(mcp):
    """unlink удаляет связь в обе стороны; повтор → notfound."""
    seed_bug_and_fact(mcp)
    ok(mcp, "link", subject_key="bug:crypto:alignment",
       predicate="related-to", object_key="fact:platform:android")

    r = ok(mcp, "unlink", subject_key="bug:crypto:alignment",
           predicate="related-to", object_key="fact:platform:android")
    assert r["result"] == "unlinked"

    g = ok(mcp, "graph", key="bug:crypto:alignment")
    assert g["edges"] == []

    r = ok(mcp, "unlink", subject_key="bug:crypto:alignment",
           predicate="related-to", object_key="fact:platform:android")
    assert r["result"] == "notfound"


# --- С8. Словарь: добавление темы и fuzzy-поиск -----------------------------------
def test_s8_vocab_add_and_fuzzy(mcp):
    """Новая тема сразу доступна для нормализации и находится fuzzy-поиском."""
    r = ok(mcp, "vocab_add", module="platform", topic="rust bindings")
    assert r["result"] == "added"

    r = ok(mcp, "normalize_key", raw="fact:platform:rust bindings")
    assert r["canonical_key"] == "fact:platform:rust-bindings"

    found = ok(mcp, "vocab_find", query="rust-binding")
    assert "rust-bindings" in found["topics"]

    # повторное добавление не дублирует
    assert ok(mcp, "vocab_add", module="platform",
              topic="rust-bindings")["result"] == "exists"


# --- С9. Канонизация и контрактная подсказка при ошибке ---------------------------
def test_s9_normalize_error_hint(mcp):
    """Неизвестный модуль — контрактная ошибка invalid с подсказкой."""
    r = tool(mcp, "normalize_key", raw="fact:memo:index")
    assert r["error"]["code"] == -32000
    msg = r["error"]["message"]
    assert msg.startswith("error$") and "CLASS=invalid" in msg
    assert "ACTION=" in msg

    # неверная арность
    r = tool(mcp, "normalize_key", raw="a:b:c:d")
    assert r["error"]["code"] == -32000

    # валидный ключ (тема добавлена в словарь)
    ok(mcp, "vocab_add", module="memory", topic="index-cache")
    r = ok(mcp, "normalize_key", raw="BUG:memory:index cache")
    assert r["canonical_key"] == "bug:memory:index-cache"


# --- С10. Снимок и восстановление контекста после сжатия --------------------------
def test_s10_context_dump_restore(mcp):
    """dump_context перед сжатием; restore_context возвращает снимок."""
    seed_bug_and_fact(mcp)

    r = ok(mcp, "dump_context", task="Fix TASK-42", milestone="M1",
           keys=["bug:crypto:alignment"], hypotheses=["h1", "h2"])
    assert r["key"] == "proc:memory:context-snapshot"
    cid = r["context_id"]
    assert cid

    recs = ok(mcp, "restore_context", context_id=cid)["records"]
    assert recs and recs[0]["key"] == "proc:memory:context-snapshot"
    assert recs[0]["type"] == "proc"


# --- С11. Безопасный GC: dry-run ничего не удаляет --------------------------------
def test_s11_gc_dry_run_safe(mcp):
    """Тиринг классифицирует, но запись остаётся (дефолт gc — не удалять)."""
    ok(mcp, "vocab_add", module="crypto", topic="alignment")
    ok(mcp, "safe_store", key="bug:crypto:alignment", type="bug",
       summary="Low importance historical bug note.", importance=0.05)

    st = ok(mcp, "gc", dry_run=True, archive=False)
    assert st["dry_run"] is True
    assert st["hot"] + st["warm"] + st["cold"] >= 1

    assert ok(mcp, "exists", key="bug:crypto:alignment")["exists"] is True

    # даже execute без archive ничего не архивирует и не удаляет
    st = ok(mcp, "gc", dry_run=False, archive=False)
    assert st.get("archived", 0) == 0
    assert ok(mcp, "exists", key="bug:crypto:alignment")["exists"] is True


# --- С12. Purge: явное удаление с очисткой индексов и связей ----------------------
def test_s12_purge_cleanup(mcp, store):
    """purge убирает запись, её связи и постинг-списки; статистика уменьшается."""
    ok(mcp, "vocab_add", module="crypto", topic="alignment")
    ok(mcp, "vocab_add", module="platform", topic="android")
    ok(mcp, "safe_store", key="bug:crypto:alignment", type="bug",
       summary="Quantum error correction fails on shared buffers.", importance=0.7)
    ok(mcp, "safe_store", key="fact:platform:android", type="fact",
       summary="Android ships shared quantum runtime libraries.", importance=0.9)
    ok(mcp, "link", subject_key="bug:crypto:alignment",
       predicate="related-to", object_key="fact:platform:android")

    r = ok(mcp, "purge", keys=["bug:crypto:alignment"])
    assert r["removed"] == ["bug:crypto:alignment"]

    assert ok(mcp, "exists", key="bug:crypto:alignment")["exists"] is False

    g = ok(mcp, "graph", key="fact:platform:android")
    assert "bug:crypto:alignment" not in g["nodes"]

    keys = [x["key"] for x in ok(mcp, "search", query="quantum", limit=10)["records"]]
    assert "bug:crypto:alignment" not in keys

    st = ok(mcp, "stats")
    assert st["records"] == 1

    # повторный purge — missing
    assert ok(mcp, "purge", keys=["bug:crypto:alignment"])["missing"] == [
        "bug:crypto:alignment"]