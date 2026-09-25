"""Контракт ошибок MCP-memory.

Формат: ``error$<CODE> | CLASS=<class> | DESC=<human> | ACTION=<hint> | RETRY=<policy>``

Классы ошибок и политики ретраев (согласовано с владельцем + Алисой):
- ``size-limit``  — ретраи запрещены (данные превышают ограничение схемы/движка);
- ``busy-io``     — движок занят/конфликт записи; до 3 автоматических ретраев;
- ``internal``    — непредвиденный сбой; 1 ретрай + эскалация;
- ``invalid``     — неверный запрос агента (арность ключа, отсутствующий vocab-терм);
  ACTION — рекомендация агенту, не директива.
"""

from __future__ import annotations


class MemoryError(Exception):
    """Базовая ошибка модуля памяти с контрактным форматом."""

    def __init__(self, code: str, cls: str, desc: str, action: str, retry: str):
        self.code = code
        self.cls = cls
        self.desc = desc
        self.action = action
        self.retry = retry
        super().__init__(self.render())

    def render(self) -> str:
        return (
            "error$%s | CLASS=%s | DESC=%s | ACTION=%s | RETRY=%s"
            % (self.code, self.cls, self.desc, self.action, self.retry)
        )

    def to_dict(self) -> dict:
        return {
            "code": self.code,
            "class": self.cls,
            "desc": self.desc,
            "action": self.action,
            "retry": self.retry,
        }


def size_limit(desc: str, action: str = "сократите данные или разбейте на несколько записей") -> MemoryError:
    return MemoryError("SIZE_LIMIT", "size-limit", desc, action, "none")


def busy_io(desc: str = "движок занят конкурирующей записью", action: str = "повторите операцию") -> MemoryError:
    return MemoryError("BUSY_IO", "busy-io", desc, action, "retry up to 3")


def internal(desc: str, action: str = "сообщите оператору") -> MemoryError:
    return MemoryError("INTERNAL", "internal", desc, action, "retry once, then escalate")


def invalid(desc: str, action: str) -> MemoryError:
    return MemoryError("INVALID", "invalid", desc, action, "none")


def parse(text: str):
    """Разбор контрактной строки обратно в MemoryError (для тестов)."""
    m = MemoryError("?", "?", "?", "?", "?")
    for part in text.split("|"):
        part = part.strip()
        if part.startswith("error$"):
            m.code = part[6:]
        elif part.startswith("CLASS="):
            m.cls = part[6:]
        elif part.startswith("DESC="):
            m.desc = part[5:]
        elif part.startswith("ACTION="):
            m.action = part[7:]
        elif part.startswith("RETRY="):
            m.retry = part[6:]
    return m