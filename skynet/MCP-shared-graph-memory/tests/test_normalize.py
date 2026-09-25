"""Тесты канонизации ключей и словаря."""

import pytest

from mcp.errors import MemoryError
from mcp.normalize import Normalizer, RECORD_TYPES


def make_norm(**kw):
    n = Normalizer()
    n.add_module("crypto")
    n.add_module("testing")
    n.add_topic("alignment")
    n.add_topic("split-monolith")
    return n


def test_record_types_constant():
    assert set(RECORD_TYPES) == {"decision", "bug", "proc", "bottleneck", "fact", "event"}


def test_default_domain_modules():
    """Доменные модули схемы знаний доступны сразу (без vocab_add)."""
    n = Normalizer()
    n.add_topic("knowledge-map")
    for module in ("meta", "practice", "swarm", "coordination", "todo", "archive"):
        assert n.normalize("fact:%s:knowledge-map" % module).startswith("fact:%s:" % module)


def test_normalize_basic():
    n = make_norm()
    assert n.normalize("bug:crypto:alignment-arm64") == "bug:crypto:alignment"
    assert n.normalize("Bug:Crypto:Alignment-ARM64") == "bug:crypto:alignment"
    assert n.normalize("fact:testing:split-monolith") == "fact:testing:split-monolith"


def test_normalize_strip_stopwords():
    n = make_norm()
    # артикли/предлоги убираются; цифры (версии/даты) убираются
    assert n.normalize("bug:crypto:the alignment of the v0.14") == "bug:crypto:alignment"
    assert n.normalize("decision:testing:split the monolith") == "decision:testing:split-monolith"


def test_normalize_wrong_arity():
    n = make_norm()
    for bad in ("a:b", "a:b:c:d", "", "bug"):
        with pytest.raises(MemoryError):
            n.normalize(bad)


def test_normalize_unknown_type():
    n = make_norm()
    with pytest.raises(MemoryError) as ei:
        n.normalize("epic:crypto:alignment")
    assert "тип записи" in str(ei.value)


def test_normalize_unknown_module():
    n = make_norm()
    with pytest.raises(MemoryError) as ei:
        n.normalize("bug:nosuchmod:alignment")
    assert "модуль" in str(ei.value)
    assert "vocab_add" in str(ei.value)


def test_normalize_unknown_topic():
    n = make_norm()
    with pytest.raises(MemoryError) as ei:
        n.normalize("bug:crypto:no-such-topic")
    assert "тема" in str(ei.value)
    assert "vocab_add" in str(ei.value)


def test_normalize_topic_too_long():
    n = make_norm()
    with pytest.raises(MemoryError):
        n.normalize("bug:crypto:" + "x" * 40)


def test_normalize_empty_topic():
    n = make_norm()
    with pytest.raises(MemoryError):
        n.normalize("bug:crypto:the")


def test_fuzzy_hints():
    n = make_norm()
    n.add_topic("arm64-buffer")
    with pytest.raises(MemoryError) as ei:
        n.normalize("bug:crypto:arm64-bufer")  # опечатка
    assert "похожие" in str(ei.value)


def test_module_case_underscore():
    n = Normalizer()
    assert n.add_module("Crypto_Module") == "crypto-module"
    assert "crypto-module" in n.modules


def test_fuzzy_modules_and_topics():
    n = make_norm()
    n.add_topic("alignment")
    assert n.fuzzy_modules("crypt")  # не пусто
    assert n.fuzzy_topics("aligment")  # не пусто