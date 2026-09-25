"""Тесты низкоуровневого биндинга libmdbx."""

import os
import tempfile

import pytest

from mcp_memory import libmdbx as mdbx
from mcp_memory.store import _pack_u64, _unpack_u64


def test_constants():
    assert mdbx.DB_DEFAULTS == 0
    assert mdbx.DUPSORT == 0x04
    assert mdbx.INTEGERKEY == 0x08
    assert mdbx.DUPFIXED == 0x10
    assert mdbx.INTEGERDUP == 0x20
    assert mdbx.CREATE == 0x40000
    assert mdbx.NOSUBDIR == 0x4000
    assert mdbx.RC_SUCCESS == 0
    assert mdbx.RC_RESULT_TRUE == -1
    assert mdbx.RC_NOTFOUND == -30798
    assert mdbx.RC_KEYEXIST == -30799
    assert mdbx.CURSOR_NEXT == 8
    assert mdbx.CURSOR_SET_RANGE == 17


def test_strerror_and_version():
    assert isinstance(mdbx.strerror(0), str)
    assert mdbx.strerror(mdbx.RC_NOTFOUND)
    assert mdbx.version_string()


def test_env_create_open_close():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "e.mdbx")
    env = mdbx.Env(p, maxdbs=8)
    assert env.maxkeysize() > 100
    assert env.maxvalsize() > 1000
    env.close()
    # повторное открытие работает
    env2 = mdbx.Env(p, maxdbs=8)
    env2.close()


def test_put_get_delete():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "e.mdbx")
    env = mdbx.Env(p, maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("t", mdbx.DB_DEFAULTS | mdbx.CREATE)
        with env.begin() as txn:
            txn.put(dbi, b"k1", b"v1")
        with env.begin(readonly=True) as txn:
            rc, val = txn.get(dbi, b"k1")
            assert rc == mdbx.RC_SUCCESS and val == b"v1"
        with env.begin() as txn:
            assert txn.delete(dbi, b"k1") is True
        with env.begin(readonly=True) as txn:
            rc, val = txn.get(dbi, b"k1")
            assert rc == mdbx.RC_NOTFOUND and val is None
    finally:
        env.close()


def test_put_nooverwrite():
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("t", mdbx.DB_DEFAULTS | mdbx.CREATE)
        with env.begin() as txn:
            txn.put(dbi, b"k", b"a", mdbx.PUT_NOOVERWRITE)
            with pytest.raises(mdbx.LibmdbxError) as ei:
                txn.put(dbi, b"k", b"b", mdbx.PUT_NOOVERWRITE)
            assert ei.value.rc == mdbx.RC_KEYEXIST
    finally:
        env.close()


def test_dupsort_integerdup_postings():
    """Постинг-список: term -> множество uint64 id (INTEGERDUP)."""
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        flags = mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP | mdbx.CREATE
        with env.begin() as txn:
            dbi = txn.open_dbi("inv", flags)
        with env.begin() as txn:
            with txn.cursor(dbi) as cur:
                assert cur.put_nodupe(b"term", _pack_u64(5)) is True
                assert cur.put_nodupe(b"term", _pack_u64(1)) is True
                assert cur.put_nodupe(b"term", _pack_u64(5)) is False
                assert cur.count() == 2
        with env.begin(readonly=True) as txn:
            with txn.cursor(dbi) as cur:
                rc, _, _ = cur.get(mdbx.CURSOR_SET, b"term")
                assert rc == mdbx.RC_SUCCESS
                got = []
                rc, _, dval = cur.get(mdbx.CURSOR_FIRST_DUP)
                while rc == mdbx.RC_SUCCESS:
                    got.append(_unpack_u64(dval))
                    rc, _, dval = cur.get(mdbx.CURSOR_NEXT_DUP)
                assert got == [1, 5]  # сортировка по возрастанию
        with env.begin() as txn:
            assert txn.delete(dbi, b"term", _pack_u64(1)) is True
            assert txn.delete(dbi, b"term", _pack_u64(1)) is False
    finally:
        env.close()


def test_integerkey_table():
    """INTEGERKEY: числовые ключи, range-поиск."""
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("id2k", mdbx.INTEGERKEY | mdbx.CREATE)
        with env.begin() as txn:
            for i in (10, 1, 7):
                txn.put(dbi, _pack_u64(i), ("rec%d" % i).encode())
        with env.begin(readonly=True) as txn:
            rc, val = txn.get(dbi, _pack_u64(7))
            assert val == b"rec7"
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                assert _unpack_u64(k) == 1
    finally:
        env.close()


def test_cursor_count_all():
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("t", mdbx.DB_DEFAULTS | mdbx.CREATE)
        with env.begin() as txn:
            for i in range(20):
                txn.put(dbi, b"k%d" % i, b"v")
        with env.begin(readonly=True) as txn:
            assert txn.count(dbi) == 20
    finally:
        env.close()


def test_get_empty_value():
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("t", mdbx.DB_DEFAULTS | mdbx.CREATE)
        with env.begin() as txn:
            txn.put(dbi, b"k", b"")
        with env.begin(readonly=True) as txn:
            rc, val = txn.get(dbi, b"k")
            assert rc == mdbx.RC_SUCCESS and val == b""
    finally:
        env.close()


def test_env_context_manager():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "e.mdbx")
    with mdbx.Env(p, maxdbs=8) as env:
        assert env.maxkeysize() > 0


def test_cursor_put_direct():
    d = tempfile.mkdtemp()
    env = mdbx.Env(os.path.join(d, "e.mdbx"), maxdbs=8)
    try:
        with env.begin() as txn:
            dbi = txn.open_dbi("t", mdbx.DB_DEFAULTS | mdbx.CREATE)
        with env.begin() as txn:
            with txn.cursor(dbi) as cur:
                cur.put(b"a", b"1")
                cur.put(b"b", b"2")
        with env.begin(readonly=True) as txn:
            rc, val = txn.get(dbi, b"b")
            assert val == b"2"
    finally:
        env.close()


def test_load_library_fallback():
    try:
        lib = mdbx.load_library("/nonexistent/libmdbx.so")
        assert lib is not None
    except OSError:
        pass  # системной libmdbx.so нет — поведение корректно