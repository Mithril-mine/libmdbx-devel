"""Канонизация ключей и контролируемый словарь (vocab).

Правила по SKILL_mcp_memory_mdbx.md §2:
- ключ — ровно 3 сегмента ``{тип}:{модуль}:{тема}``;
- типы: decision, bug, proc, bottleneck, fact, event;
- модуль и тема — из контролируемого словаря;
- нормализация детерминированная: lowercase, kebab-case, без стоп-слов,
  без версий/дат в теме, тема <= 30 символов, 1..3 слова.
"""

from __future__ import annotations

import difflib
import re
import unicodedata

from .errors import MemoryError, invalid

RECORD_TYPES = ("decision", "bug", "proc", "bottleneck", "fact", "event")

DEFAULT_MODULES = (
    # домен meta (нулевой): знания о знаниях — схема, процедуры, метрики
    "meta",
    # домен general: общие практики и принципы (Кайдзен, методологии)
    "practice",
    # домен swarm: рой и его протоколы (доки — в skynet-draft)
    "swarm",
    "coordination",
    # домены неклассифицированного: ожидают разбора / устаревшее (legacy)
    "todo",
    "archive",
    # домен project (libmdbx): профильные модули проекта
    "crypto",
    "network",
    "storage",
    "ui",
    "build",
    "testing",
    "review",
    "platform",
    "core",
    "memory",
)

# Артикли, предлоги, союзы (en+ru) — не входят в тему.
STOP_WORDS = {
    # english
    "the", "a", "an", "on", "of", "in", "at", "for", "to", "with", "and",
    "or", "but", "is", "are", "was", "were", "be", "by", "from", "as",
    "via", "into", "over", "under", "about", "after", "before",
    # russian
    "и", "в", "во", "на", "с", "со", "к", "ко", "по", "от", "о", "об",
    "для", "из", "при", "до", "не", "как", "или", "а", "но", "то", "это",
}

# Символы, недопустимые в теме после нормализации.
_BAD_CHARS = re.compile(r"[^a-zа-я0-9-]")
_DIGITS = re.compile(r"\d")


def _fold(text: str) -> str:
    """Lowercase + нормализация unicode (NFKC) без диакритики."""
    text = unicodedata.normalize("NFKC", text)
    return text.lower()


def _words(text: str) -> list:
    return re.split(r"[^a-zа-я0-9]+", _fold(text))


class Normalizer:
    """Канонизатор ключей, зависящий от текущего словаря (модули/темы)."""

    def __init__(self):
        self.modules = set(DEFAULT_MODULES)
        self.topics = set()

    def add_module(self, module: str) -> str:
        m = _fold(module).strip().replace("_", "-").replace(" ", "-")
        m = re.sub(r"-{2,}", "-", m).strip("-")
        if not m:
            raise invalid("пустой модуль", "укажите непустое имя модуля")
        if m not in self.modules:
            self.modules.add(m)
        return m

    def add_topic(self, topic: str) -> str:
        t = self._normalize_topic(topic)
        if t not in self.topics:
            self.topics.add(t)
        return t

    def remove_topic(self, topic: str) -> bool:
        t = self._normalize_topic(topic)
        return t in self.topics and (self.topics.remove(t) or True)

    # --- внутренняя нормализация темы -------------------------------------
    def _normalize_topic(self, raw: str) -> str:
        words = [w for w in _words(raw) if w and w not in STOP_WORDS]
        words = [w for w in words if not _DIGITS.search(w)]  # без версий/дат
        words = words[:3]  # максимум 3 слова
        topic = "-".join(words)
        topic = _BAD_CHARS.sub("-", topic)
        topic = re.sub(r"-{2,}", "-", topic).strip("-")
        if len(topic) > 30:
            raise invalid(
                "тема длиннее 30 символов после нормализации: %r" % topic,
                "сократите тему до 30 символов",
            )
        if not topic:
            raise invalid(
                "тема пуста после нормализации",
                "укажите содержательные слова темы",
            )
        return topic

    # --- публичные операции ------------------------------------------------
    def normalize(self, raw: str):
        """Возвращает канонический ключ или бросает MemoryError(invalid)."""
        segments = raw.split(":")
        if len(segments) != 3:
            raise invalid(
                "ключ должен иметь ровно 3 сегмента {тип}:{модуль}:{тема}, "
                "получено %d: %r" % (len(segments), raw),
                "приведите ключ к виду тип:модуль:тема",
            )
        rtype, module, topic = segments
        rtype = rtype.strip().lower()
        if rtype not in RECORD_TYPES:
            raise invalid(
                "неизвестный тип записи %r (допустимые: %s)"
                % (rtype, ", ".join(RECORD_TYPES)),
                "используйте один из допустимых типов",
            )
        module_n = _fold(module).strip().replace("_", "-").replace(" ", "-")
        module_n = re.sub(r"-{2,}", "-", module_n).strip("-")
        if module_n not in self.modules:
            hints = difflib.get_close_matches(module_n, sorted(self.modules), n=3)
            hint = " похожие: %s" % ", ".join(hints) if hints else ""
            raise invalid(
                "модуль %r отсутствует в словаре%s" % (module_n, hint),
                "вызовите vocab_add(module=..., topic=...) или используйте существующий модуль",
            )
        topic_n = self._normalize_topic(topic)
        if topic_n not in self.topics:
            hints = difflib.get_close_matches(topic_n, sorted(self.topics), n=3)
            hint = " похожие: %s" % ", ".join(hints) if hints else ""
            raise invalid(
                "тема %r отсутствует в словаре%s" % (topic_n, hint),
                "вызовите vocab_add(module=<mod>, topic=%r)" % topic_n,
            )
        return "%s:%s:%s" % (rtype, module_n, topic_n)

    def fuzzy_topics(self, query: str, n: int = 5) -> list:
        q = self._normalize_topic(query)
        return difflib.get_close_matches(q, sorted(self.topics), n=n, cutoff=0.6)

    def fuzzy_modules(self, query: str, n: int = 5) -> list:
        q = _fold(query).strip()
        return difflib.get_close_matches(q, sorted(self.modules), n=n, cutoff=0.6)