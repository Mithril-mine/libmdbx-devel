"""Тесты контракта ошибок."""

from mcp_memory import errors
from mcp_memory.errors import MemoryError, invalid, busy_io, internal, size_limit, parse


def test_format():
    e = MemoryError("SIZE_LIMIT", "size-limit", "слишком длинное значение",
                    "сократите данные", "none")
    text = str(e)
    assert text.startswith("error$SIZE_LIMIT |")
    assert "CLASS=size-limit" in text
    assert "DESC=" in text
    assert "ACTION=" in text
    assert "RETRY=none" in text


def test_classes():
    assert invalid("x", "y").cls == "invalid"
    assert busy_io().cls == "busy-io"
    assert internal("x").cls == "internal"
    assert size_limit("x").cls == "size-limit"


def test_retry_policies():
    assert invalid("x", "y").retry == "none"
    assert busy_io().retry == "retry up to 3"
    assert internal("x").retry == "retry once, then escalate"
    assert size_limit("x").retry == "none"


def test_to_dict():
    d = invalid("описание", "действие").to_dict()
    assert d["code"] == "INVALID"
    assert d["class"] == "invalid"
    assert d["desc"] == "описание"
    assert d["action"] == "действие"
    assert d["retry"] == "none"


def test_parse_roundtrip():
    e = MemoryError("BUSY_IO", "busy-io", "движок занят", "повторите", "retry up to 3")
    p = parse(str(e))
    assert p.code == "BUSY_IO"
    assert p.cls == "busy-io"
    assert p.desc == "движок занят"
    assert p.action == "повторите"
    assert p.retry == "retry up to 3"


def test_errors_module_export():
    assert errors.MemoryError is MemoryError