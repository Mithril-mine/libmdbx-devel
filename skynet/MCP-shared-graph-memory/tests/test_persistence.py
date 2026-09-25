"""Тесты персистентности: переоткрытие Store сохраняет всё."""

from mcp import Store
from tests.conftest import seed_vocab


def _seed(path):
    s = Store(path)
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi")])
    s.safe_store("bug:crypto:alignment-arm64", "bug",
                 "Segfault misaligned NEON.", 0.7)
    s.safe_store("fact:platform:android-abi", "fact", "Android ABIs.", 0.9)
    s.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    s.close()


def test_persistence_full(store_path):
    _seed(store_path)
    s = Store(store_path)
    try:
        assert s.exists("bug:crypto:alignment")
        assert s.lookup("segfault") == ["bug:crypto:alignment"]
        assert len(s.graph("bug:crypto:alignment", depth=1)["edges"]) == 1
        assert s.stats()["next_id"] == 3
        # vocab сохранён
        assert "android-abi" in s.norm.topics
    finally:
        s.close()


def test_persistence_after_write(store_path):
    _seed(store_path)
    s = Store(store_path)
    try:
        s.safe_store("bug:crypto:alignment", "bug", "Updated text.", 0.5)
        assert s.get_record("bug:crypto:alignment")["summary"] == "Updated text."
    finally:
        s.close()
    s2 = Store(store_path)
    try:
        assert s2.get_record("bug:crypto:alignment")["summary"] == "Updated text."
    finally:
        s2.close()