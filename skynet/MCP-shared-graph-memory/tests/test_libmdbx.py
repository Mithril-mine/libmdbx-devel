"""Тесты низкоуровневого биндинга libmdbx."""

import os

import pytest

from mcp import libmdbx as mdbx
from mcp.store import _pack_u64, _unpack_u64


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
    assert mdbx.MDBX_SAFE_NOSYNC == 0x10000
    assert mdbx.MDBX_OPT_MAX_DB == 0
    assert mdbx.MDBX_OPT_SYNC_BYTES == 2
    assert mdbx.MDBX_OPT_SYNC_PERIOD == 3


def test_env_sync_flags_and_options(tmp_path, monkeypatch):
    """Env открывается с SAFE_NOSYNC и порогами sync_bytes/sync_period."""
    open_flags = {}
    options = []

    def fake_open(env, path, flags, mode):
        open_flags["flags"] = flags
        open_flags["mode"] = mode
        return 0

    def fake_set_option(env, option, value):
        options.append((option, value))
        return 0

    class _FakeLib:
        def __init__(self):
            self.mdbx_env_create = lambda out: 0
            self.mdbx_env_set_option = fake_set_option
            self.mdbx_env_open = fake_open

    monkeypatch.setattr(mdbx, "_get_lib", lambda: _FakeLib())
    mdbx.Env(os.path.join(str(tmp_path), "t.mdbx"),
             sync_bytes=64 << 20, sync_period=60)
    assert open_flags["flags"] & mdbx.MDBX_SAFE_NOSYNC
    opts = dict(options)
    assert opts[mdbx.MDBX_OPT_SYNC_BYTES] == 64 << 20
    assert opts[mdbx.MDBX_OPT_SYNC_PERIOD] == 60


def test_strerror_and_version():
    assert isinstance(mdbx.strerror(0), str)
    assert mdbx.strerror(mdbx.RC_NOTFOUND)
    v = mdbx.version_string()
    assert not v.startswith("?"), "version_string() не прочитал mdbx_version"
    assert v.split(".", 1)[0].isdigit()
    assert mdbx.build_string()


def test_env_create_flag(tmp_path):
    """mode=0 открывает существующее без создания (mdbx.h env_open)."""
    d = str(tmp_path)
    p = os.path.join(d, "e.mdbx")
    with pytest.raises(mdbx.LibmdbxError):
        mdbx.Env(p, maxdbs=8, create=False)
    env = mdbx.Env(p, maxdbs=8, create=True)
    env.close()
    env2 = mdbx.Env(p, maxdbs=8, create=False)
    env2.close()


def test_env_create_open_close(tmp_path):
    d = str(tmp_path)
    p = os.path.join(d, "e.mdbx")
    env = mdbx.Env(p, maxdbs=8)
    assert env.maxkeysize() > 100
    assert env.maxvalsize() > 1000
    env.close()
    # повторное открытие работает
    env2 = mdbx.Env(p, maxdbs=8)
    env2.close()


def test_put_get_delete(tmp_path):
    d = str(tmp_path)
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


def test_put_nooverwrite(tmp_path):
    d = str(tmp_path)
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


def test_dupsort_integerdup_postings(tmp_path):
    """Постинг-список: term -> множество uint64 id (INTEGERDUP)."""
    d = str(tmp_path)
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


def test_integerkey_table(tmp_path):
    """INTEGERKEY: числовые ключи, range-поиск."""
    d = str(tmp_path)
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


def test_cursor_count_all(tmp_path):
    d = str(tmp_path)
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


def test_get_empty_value(tmp_path):
    d = str(tmp_path)
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


def test_env_context_manager(tmp_path):
    d = str(tmp_path)
    p = os.path.join(d, "e.mdbx")
    with mdbx.Env(p, maxdbs=8) as env:
        assert env.maxkeysize() > 0


def test_cursor_put_direct(tmp_path):
    d = str(tmp_path)
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


@pytest.mark.skipif(not hasattr(os, "fork"),
                    reason="os.fork() отсутствует на Windows")
def test_env_pid_guard_rejects_forked_child(tmp_path):
    """D3/N11: env не fork-safe — потомок получает явную ошибку, не коррупцию.

    Унаследованный handle в дочернем процессе повредил бы LCK/reader-слоты
    молча; begin()/sync()/reader_check() сверяют pid и отказывают.
    """
    d = str(tmp_path / "pid.mdbx")
    with mdbx.Env(d, maxdbs=4, readonly=False,
                  sync_mode="safe_nosync") as env:
        with env.begin() as txn:
            txn.open_dbi("t", mdbx.CREATE | mdbx.DB_DEFAULTS)
        pid = os.fork()
        if pid == 0:
            code = 0
            try:
                env.begin(readonly=True)
                code = 1  # guard не сработал — ошибка теста
            except RuntimeError as e:
                code = 0 if "fork-safe" in str(e) else 2
            try:
                env.sync(force=False, nonblock=True)
                code = code or 3  # guard не сработал
            except RuntimeError as e:
                if "fork-safe" not in str(e):
                    code = code or 5  # не та ошибка
            try:
                env.reader_check()
                code = code or 4  # guard не сработал
            except RuntimeError as e:
                if "fork-safe" not in str(e):
                    code = code or 6  # не та ошибка
            os._exit(code)
        _, status = os.waitpid(pid, 0)
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, \
            "потомок должен получить RuntimeError из всех точек входа"
        # родитель не затронут
        with env.begin(readonly=True) as txn:
            pass
