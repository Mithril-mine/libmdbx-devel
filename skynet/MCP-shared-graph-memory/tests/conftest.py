import ctypes
import os
import platform
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _harden_glibc_heap() -> None:
    """Enable glibc heap hardening (MALLOC_CHECK_=7, MALLOC_PERTURB_=42).

    glibc reads the env vars at the first malloc, so setting them here (inside
    an already-running interpreter) would be too late; mallopt() applies at any
    time and gives plain `python3 -m pytest` runs the same protection the CI
    and CTest get from the environment. Freed chunks are filled with ~42 and
    allocation-consistency violations abort, which turns use-after-free of the
    binding's marshalled buffers into an immediate, local failure.
    """
    try:
        if platform.libc_ver()[0] != "glibc":
            return  # musl/macOS/Windows: only the caller-set env vars apply
        libc = ctypes.CDLL(None)
        libc.mallopt.restype = ctypes.c_int
        libc.mallopt.argtypes = [ctypes.c_int, ctypes.c_int]
        M_CHECK_ACTION, M_PERTURB = -5, -6
        if libc.mallopt(M_CHECK_ACTION, 7) != 1 or libc.mallopt(M_PERTURB, 42) != 1:
            raise RuntimeError("mallopt() refused the hardening values")
    except Exception:
        pass  # best-effort: never block the suite on a hardening setup failure


_harden_glibc_heap()

from mcp import Store  # noqa: E402


@pytest.fixture
def store(tmp_path):
    # tmp_path чистится pytest автоматически: никаких утечек в /tmp
    # (mkdtemp без cleanup переполнял диск — сотни каталогов с БД+backups)
    s = Store(str(tmp_path / "test.mdbx"))
    yield s
    s.close()


@pytest.fixture
def store_path(tmp_path):
    return str(tmp_path / "test.mdbx")


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
