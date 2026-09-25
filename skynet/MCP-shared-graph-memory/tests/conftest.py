import os
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mcp_memory import Store  # noqa: E402


@pytest.fixture
def store():
    d = tempfile.mkdtemp()
    s = Store(os.path.join(d, "test.mdbx"))
    yield s
    s.close()


@pytest.fixture
def store_path():
    d = tempfile.mkdtemp()
    return os.path.join(d, "test.mdbx")


def seed_vocab(s: Store, pairs):
    for module, topic in pairs:
        s.vocab_add(module, topic)


def seed_crypto(s: Store):
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi"), ("crypto", "align")])
    r1 = s.safe_store("bug:crypto:alignment-arm64", "bug",
                      "Segfault in AES-CBC on arm64 at buffer over 4KB. "
                      "Cause: misaligned NEON. Fix: align 16 bytes.", 0.7)
    r2 = s.safe_store("fact:platform:android-abi", "fact",
                      "Android builds: NDK r26, ABIs armeabi-v7a/arm64-v8a/x86_64.", 0.9)
    return r1, r2