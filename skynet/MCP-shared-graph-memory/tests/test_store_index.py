"""Тесты поиска: recall / search / lookup / инвертированный индекс."""

from tests.conftest import seed_crypto, seed_vocab


def setup(s):
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi")])
    s.safe_store("bug:crypto:alignment-arm64", "bug",
                 "Segfault in AES-CBC on arm64 at buffer over 4KB. "
                 "Cause: misaligned NEON. Fix: align 16 bytes.", 0.7)
    s.safe_store("fact:platform:android-abi", "fact",
                 "Android builds: NDK r26, ABIs armeabi-v7a/arm64-v8a/x86_64.", 0.9)
    return s


def test_recall_prefix(store):
    setup(store)
    rows = store.recall("bug:", limit=5)
    assert [r["key"] for r in rows] == ["bug:crypto:alignment"]
    assert rows[0]["type"] == "bug"
    assert rows[0]["score"] > 0


def test_recall_full_prefix_and_limit(store):
    setup(store)
    rows = store.recall("", limit=10)
    assert len(rows) == 2
    rows1 = store.recall("bug:", limit=0)
    assert len(rows1) == 1  # clamp к 1


def test_recall_pattern_star(store):
    setup(store)
    rows = store.recall("bug:crypto:*", limit=5)
    assert len(rows) == 1


def test_lookup_single(store):
    setup(store)
    keys = store.lookup("segfault")
    assert keys == ["bug:crypto:alignment"]


def test_lookup_multiword_intersection(store):
    setup(store)
    keys = store.lookup("segfault arm64")
    assert keys == ["bug:crypto:alignment"]
    keys = store.lookup("segfault android")
    assert keys == []


def test_lookup_unknown_term(store):
    setup(store)
    assert store.lookup("zzzznope") == []


def test_search_ranks(store):
    setup(store)
    rows = store.search("arm64", limit=5)
    keys = [r["key"] for r in rows]
    assert "bug:crypto:alignment" in keys


def test_search_limit_clamp(store):
    setup(store)
    rows = store.search("arm64", limit=100)
    assert len(rows) <= 10


def test_update_terms_on_merge(store):
    setup(store)
    # merge без слова segfault -> терм убирается из inverted
    store.safe_store("bug:crypto:alignment", "bug",
                     "Fixed by aligning buffers to 16 bytes. Status: fixed.", 0.6)
    assert store.lookup("segfault") == []
    assert store.lookup("aligning") == ["bug:crypto:alignment"]


def test_inverted_intid_postings(store):
    """Проверка, что постинг-списки хранятся как uint64 id (INTEGERDUP)."""
    setup(store)
    with store.env.begin(readonly=True) as txn:
        inv = store.dbi(txn, "inverted")
        with txn.cursor(inv) as cur:
            rc, _, _ = cur.get(0)  # FIRST
            assert rc == 0
            # каждое значение — ровно 8 байт (uint64)
            rc, _, dval = cur.get(1)  # FIRST_DUP
            assert len(dval) == 8