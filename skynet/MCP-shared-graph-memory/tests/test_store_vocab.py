"""Тесты словаря vocab."""

import pytest

from mcp_memory import Store
from mcp_memory.errors import MemoryError
from mcp_memory.normalize import DEFAULT_MODULES


def test_default_modules_present(store):
    for m in ("crypto", "network", "storage", "ui", "build", "testing",
              "review", "platform", "core", "memory"):
        assert m in store.norm.modules


def test_vocab_add_module(store):
    r = store.vocab_add("newmod")
    assert r["result"] == "added"
    assert "module:newmod" in r["key"]
    assert "newmod" in store.norm.modules
    # повторно -> exists
    r2 = store.vocab_add("newmod")
    assert r2["result"] == "exists"


def test_vocab_add_topic(store):
    r = store.vocab_add("crypto", "buffer-alignment")
    assert r["result"] == "added"
    assert "crypto:buffer-alignment" == r["key"]
    assert "buffer-alignment" in store.norm.topics
    r2 = store.vocab_add("crypto", "buffer-alignment")
    assert r2["result"] == "exists"


def test_vocab_add_topic_normalizes(store):
    r = store.vocab_add("crypto", "Buffer Alignment")
    assert r["key"] == "crypto:buffer-alignment"


def test_vocab_find(store):
    store.vocab_add("crypto", "alignment")
    res = store.vocab_find("aligment")
    assert "alignment" in res["topics"]
    res2 = store.vocab_find("crypt")
    assert "crypto" in res2["modules"]


def test_vocab_list(store):
    store.vocab_add("crypto", "alignment")
    lst = store.vocab_list()
    assert "crypto" in lst["modules"]
    assert "alignment" in lst["topics"]


def test_vocab_persists(store_path):
    # запись в один Store, чтение из другого
    s1 = Store(store_path)
    s1.vocab_add("custom", "theme")
    s1.close()
    s2 = Store(store_path)
    try:
        assert "custom" in s2.norm.modules
        assert "theme" in s2.norm.topics
    finally:
        s2.close()


def test_normalize_uses_persisted_vocab(store_path):
    s1 = Store(store_path)
    s1.vocab_add("custom", "theme")
    s1.safe_store("bug:custom:theme", "bug", "some bug text", 0.5)
    s1.close()
    s2 = Store(store_path)
    try:
        assert s2.exists("bug:custom:theme")
    finally:
        s2.close()