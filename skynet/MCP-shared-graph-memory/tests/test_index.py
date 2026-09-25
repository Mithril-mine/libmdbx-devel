"""Тесты токенизации и SimHash."""

from mcp_memory.index import tokenize, simhash, hamming


def test_tokenize_basic():
    assert tokenize("Segfault in AES-CBC on arm64!") == ["segfault", "aes", "cbc", "arm64"]


def test_tokenize_min_length():
    assert "on" not in tokenize("on the go")
    assert "to" not in tokenize("to be or not")


def test_tokenize_stopwords_off():
    toks = tokenize("the quick and clever fox", stopwords=False)
    assert "the" in toks and "and" in toks


def test_tokenize_dedup_order():
    assert tokenize("foo foo bar") == ["foo", "bar"]


def test_simhash_deterministic():
    assert simhash("same text") == simhash("same text")


def test_simhash_zero_for_empty():
    assert simhash("") == 0


def test_hamming_identical_zero():
    assert hamming(simhash("abc"), simhash("abc")) == 0


def test_similar_texts_close():
    a = "Segfault in AES-CBC on arm64 at buffer over 4KB. Fix: align 16 bytes."
    b = "Segfault in AES-CBC on arm64 at buffer over 4KB. Fix: align 16 bytes!"
    assert hamming(simhash(a), simhash(b)) <= 3


def test_different_texts_far():
    a = "the quick brown fox jumps over the lazy dog"
    b = "pack my box with five dozen liquor jugs"
    assert hamming(simhash(a), simhash(b)) > 10


def test_text_hamming_distance_helper():
    from mcp_memory.index import text_hamming_distance
    assert text_hamming_distance("same text", "same text") == 0
    assert text_hamming_distance("different words here", "other words there") > 0