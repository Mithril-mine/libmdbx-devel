"""Токенизация и SimHash (фаза 1 без TF-IDF векторов).

- tokenize(): термы для inverted-индекса (нормализованные слова);
- simhash(): 64-битный взвешенный отпечаток текста для дедупликации
  (Hamming <= 3 трактуется как «близкий дубль», по SKILL §5.2).
"""

from __future__ import annotations

import hashlib
import re
import unicodedata

from .normalize import STOP_WORDS

_TOKEN_RE = re.compile(r"[a-zа-я0-9]{3,}")


def _fold(text: str) -> str:
    return unicodedata.normalize("NFKC", text).lower()


def tokenize(text: str, stopwords: bool = True) -> list:
    """Термы из текста: lowercase, только буквы/цифры (>=3), без стоп-слов."""
    words = _TOKEN_RE.findall(_fold(text))
    if stopwords:
        words = [w for w in words if w not in STOP_WORDS]
    # дедупликация с сохранением порядка
    seen = set()
    out = []
    for w in words:
        if w not in seen:
            seen.add(w)
            out.append(w)
    return out


def _hash64(word: str) -> int:
    return int.from_bytes(hashlib.blake2b(word.encode("utf-8"), digest_size=8).digest(), "little")


def simhash(text: str, bits: int = 64) -> int:
    """Взвешенный SimHash: знак суммы +/-hash[i]*tf_i."""
    words = _TOKEN_RE.findall(_fold(text))
    if not words:
        return 0
    vec = [0] * bits
    counts: dict = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    for w, tf in counts.items():
        h = _hash64(w)
        weight = min(tf, 4)  # насыщение веса, чтобы длинный текст не доминировал
        for i in range(bits):
            if (h >> i) & 1:
                vec[i] += weight
            else:
                vec[i] -= weight
    result = 0
    for i in range(bits):
        if vec[i] >= 0:
            result |= 1 << i
    return result


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def text_hamming_distance(a: str, b: str) -> int:
    """Прямое расстояние между отпечатками двух текстов."""
    return hamming(simhash(a), simhash(b))