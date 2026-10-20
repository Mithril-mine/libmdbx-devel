"""Тесты словаря vocab."""


from mcp import Store


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
    with Store(store_path) as s2:
        assert s2.exists("bug:custom:theme")


def test_vocab_refs_lifecycle_and_gc(store):
    """C-7: авто-тема с refs==0 вычищается gc(); ручная — защищена."""
    import json as _json
    # авто-регистрация: запись создаёт refs для модуля и темы
    store.safe_store("fact:autozone:widget", "fact", "text", 0.05)
    with store.env.begin(readonly=True) as txn:
        voc = store.dbi(txn, "vocab")
        _, v = txn.get(voc, b"fact:autozone:widget")
        meta = _json.loads(v.decode())
    assert meta["refs"] == 1 and meta["auto"] is True
    # ручная тема — auto=False (legacy-формат b"{}" трактуется как ручная)
    store.vocab_add("manualmod", "manualtopic")
    with store.env.begin(readonly=True) as txn:
        _, v = txn.get(store.dbi(txn, "vocab"), b"manualmod:manualtopic")
        meta = store._vocab_meta(v)
    assert meta["auto"] is False
    # purge → refs=0
    store.purge(["fact:autozone:widget"])
    with store.env.begin(readonly=True) as txn:
        _, v = txn.get(store.dbi(txn, "vocab"), b"fact:autozone:widget")
        assert _json.loads(v.decode())["refs"] == 0
    # gc чистит auto с refs==0; ручные не трогает
    res = store.gc(dry_run=False)
    assert res["vocab_removed"] >= 1
    with store.env.begin(readonly=True) as txn:
        voc = store.dbi(txn, "vocab")
        assert txn.get(voc, b"fact:autozone:widget")[1] is None
        assert txn.get(voc, b"module:autozone")[1] is None
        assert txn.get(voc, b"manualmod:manualtopic")[1] is not None


def test_vocab_legacy_empty_json_is_manual(store):
    """Старый формат b'{}' трактуется как ручная запись (auto=False)."""
    with store.env.begin() as txn:
        voc = store.dbi(txn, "vocab")
        txn.put(voc, b"legacymod:legacytopic", b"{}")
    assert store._vocab_meta(b"{}") == {"refs": 0, "auto": False}
    store.gc(dry_run=False)
    # легаси-запись не удалена (auto=False)
    with store.env.begin(readonly=True) as txn:
        voc = store.dbi(txn, "vocab")
        assert txn.get(voc, b"legacymod:legacytopic")[1] is not None
