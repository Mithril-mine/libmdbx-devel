"""Low-level libmdbx bindings (cffi ABI mode).

Все низкоуровневые вызовы собраны здесь; `store.py` использует только этот слой.
Документация: https://libmdbx.dqdkfa.ru/ (C API groups).
"""

from __future__ import annotations

import ctypes
from typing import Optional, Tuple

from cffi import FFI

ffi = FFI()

ffi.cdef(
    """
    typedef uint32_t MDBX_dbi;
    typedef struct { void *iov_base; size_t iov_len; } MDBX_val;

    #define MDBX_RESULT_FALSE 0
    #define MDBX_RESULT_TRUE -1
    #define MDBX_KEYEXIST -30799
    #define MDBX_NOTFOUND -30798
    #define MDBX_MAP_FULL -30792
    #define MDBX_INCOMPATIBLE -30784
    #define MDBX_BAD_VALSIZE -30781
    #define MDBX_BUSY -30778
    #define MDBX_EMULTIVAL -30421
    #define MDBX_THREAD_MISMATCH -30416
    #define MDBX_EINVAL 22

    #define MDBX_DB_DEFAULTS 0
    #define MDBX_REVERSEKEY 0x02
    #define MDBX_DUPSORT 0x04
    #define MDBX_INTEGERKEY 0x08
    #define MDBX_DUPFIXED 0x10
    #define MDBX_INTEGERDUP 0x20
    #define MDBX_REVERSEDUP 0x40
    #define MDBX_CREATE 0x40000

    #define MDBX_NOSUBDIR 0x4000
    #define MDBX_RDONLY 0x20000

    #define MDBX_UPSERT 0
    #define MDBX_NOOVERWRITE 0x10
    #define MDBX_NODUPDATA 0x20
    #define MDBX_CURRENT 0x40
    #define MDBX_ALLDUPS 0x80
    #define MDBX_APPEND 0x20000
    #define MDBX_APPENDDUP 0x40000

    #define MDBX_FIRST 0
    #define MDBX_FIRST_DUP 1
    #define MDBX_GET_BOTH 2
    #define MDBX_GET_BOTH_RANGE 3
    #define MDBX_GET_CURRENT 4
    #define MDBX_GET_MULTIPLE 5
    #define MDBX_LAST 6
    #define MDBX_LAST_DUP 7
    #define MDBX_NEXT 8
    #define MDBX_NEXT_DUP 9
    #define MDBX_NEXT_MULTIPLE 10
    #define MDBX_NEXT_NODUP 11
    #define MDBX_PREV 12
    #define MDBX_PREV_DUP 13
    #define MDBX_PREV_NODUP 14
    #define MDBX_SET 15
    #define MDBX_SET_KEY 16
    #define MDBX_SET_RANGE 17
    #define MDBX_PREV_MULTIPLE 18
    #define MDBX_SET_LOWERBOUND 19
    #define MDBX_SET_UPPERBOUND 20

    int mdbx_env_create(void **env);
    int mdbx_env_set_maxdbs(void *env, unsigned int maxdbs);
    int mdbx_env_open(void *env, const char *path, unsigned int flags,
                      unsigned int mode);
    int mdbx_env_close(void *env);
    int mdbx_txn_begin(void *env, void *parent, unsigned int flags,
                       void **txn);
    int mdbx_txn_commit(void *txn);
    void mdbx_txn_abort(void *txn);
    int mdbx_dbi_open(void *txn, const char *name, unsigned int flags,
                      MDBX_dbi *dbi);
    int mdbx_dbi_close(void *env, MDBX_dbi dbi);
    int mdbx_dbi_flags(void *txn, MDBX_dbi dbi, unsigned int *flags);
    int mdbx_put(void *txn, MDBX_dbi dbi, const MDBX_val *key,
                 const MDBX_val *data, unsigned int flags);
    int mdbx_get(void *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data);
    int mdbx_del(void *txn, MDBX_dbi dbi, const MDBX_val *key,
                 const MDBX_val *data);
    int mdbx_cursor_open(void *txn, MDBX_dbi dbi, void **cursor);
    void mdbx_cursor_close(void *cursor);
    int mdbx_cursor_get(void *cursor, MDBX_val *key, MDBX_val *data,
                        int op);
    int mdbx_cursor_put(void *cursor, const MDBX_val *key,
                        const MDBX_val *data, unsigned int flags);
    int mdbx_cursor_del(void *cursor, unsigned int flags);
    int mdbx_cursor_count(void *cursor, size_t *count);
    int mdbx_cursor_eof(void *cursor);
    const char *mdbx_strerror(int errnum);
    const char *mdbx_version_string(void);
    int mdbx_env_get_maxvalsize_ex(void *env, unsigned int flags);
    int mdbx_env_get_maxkeysize_ex(void *env);
    """
)


DEFAULT_SO = "/home/sourcecraft-ci-runner/.local/share/libmdbx-memory/build/libmdbx.so"


def load_library(path: str = None) -> object:
    """Загрузка libmdbx.so. Путь: env MDBX_SO_PATH > дефолт > системная."""
    import os as _os
    if path is None:
        path = _os.environ.get("MDBX_SO_PATH") or DEFAULT_SO
    try:
        return ffi.dlopen(path)
    except OSError:
        return ffi.dlopen("libmdbx.so")


_lib = None


def _get_lib():
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


# --- константы (значения из mdbx.h, сверены grep'ом) ---------------------------
DB_DEFAULTS = 0
DUPSORT = 0x04
INTEGERKEY = 0x08
DUPFIXED = 0x10
INTEGERDUP = 0x20
CREATE = 0x40000
NOSUBDIR = 0x4000
TXN_READWRITE = 0
TXN_RDONLY = 0x20000
PUT_UPSERT = 0
PUT_NOOVERWRITE = 0x10
PUT_NODUPDATA = 0x20
PUT_CURRENT = 0x40
PUT_ALLDUPS = 0x80

CURSOR_FIRST = 0
CURSOR_FIRST_DUP = 1
CURSOR_GET_BOTH = 2
CURSOR_GET_CURRENT = 4
CURSOR_GET_MULTIPLE = 5
CURSOR_LAST = 6
CURSOR_LAST_DUP = 7
CURSOR_NEXT = 8
CURSOR_NEXT_DUP = 9
CURSOR_NEXT_MULTIPLE = 10
CURSOR_NEXT_NODUP = 11
CURSOR_PREV = 12
CURSOR_SET = 15
CURSOR_SET_KEY = 16
CURSOR_SET_RANGE = 17

RC_SUCCESS = 0
RC_RESULT_TRUE = -1
RC_KEYEXIST = -30799
RC_NOTFOUND = -30798
RC_MAP_FULL = -30792
RC_INCOMPATIBLE = -30784
RC_BAD_VALSIZE = -30781
RC_BUSY = -30778
RC_EMULTIVAL = -30421


class LibmdbxError(RuntimeError):
    """Ошибка движка с кодом rc."""

    def __init__(self, rc: int, where: str = ""):
        self.rc = rc
        self.where = where
        msg = "%s(%s): %s" % (where, rc, strerror(rc))
        super().__init__(msg)


def strerror(rc: int) -> str:
    try:
        c = _get_lib().mdbx_strerror(rc)
        if c == ffi.NULL:
            return "unknown"
        return ffi.string(c).decode("utf-8", "replace")
    except Exception:
        return "unknown"


def version_string() -> str:
    try:
        return ffi.string(_get_lib().mdbx_version_string()).decode()
    except Exception:
        return "?"


def check(rc: int, where: str = "") -> int:
    """Проверяет код возврата: 0 и MDBX_RESULT_TRUE(-1) — не ошибки."""
    if rc == RC_SUCCESS or rc == RC_RESULT_TRUE:
        return rc
    raise LibmdbxError(rc, where)


# --- MDBX_val helper ------------------------------------------------------------
class MVal:
    """MDBX_val с удержанием буфера (cffi иначе освобождает iov_base)."""

    __slots__ = ("_buf", "ptr")

    def __init__(self, data: bytes):
        self._buf = ffi.new("uint8_t[]", data)
        self.ptr = ffi.new("MDBX_val *")
        self.ptr.iov_base = self._buf
        self.ptr.iov_len = len(data)


def _val_from_bytes(data: bytes) -> MVal:
    return MVal(data)


def _val_bytes(val: object) -> bytes:
    if val.iov_base == ffi.NULL or val.iov_len == 0:
        return b""
    return bytes(ffi.buffer(val.iov_base, val.iov_len))


class Env:
    """Обёртка над MDBX_env с автоматическим закрытием."""

    def __init__(self, path: str, maxdbs: int = 32, create: bool = True):
        self.path = path
        out = ffi.new("void **")
        check(_get_lib().mdbx_env_create(out), "mdbx_env_create")
        self._env = out[0]
        check(_get_lib().mdbx_env_set_maxdbs(self._env, maxdbs), "mdbx_env_set_maxdbs")
        flags = NOSUBDIR if create else NOSUBDIR
        rc = _get_lib().mdbx_env_open(self._env, path.encode(), flags, 0o644)
        if rc != RC_SUCCESS:
            # Окружение может существовать, но быть несовместимым — пробуем без create
            check(rc, "mdbx_env_open")

    def begin(self, readonly: bool = False, parent: Optional[object] = None) -> "Txn":
        out = ffi.new("void **")
        flags = TXN_RDONLY if readonly else TXN_READWRITE
        parent_ptr = ffi.NULL if parent is None else parent._txn
        check(
            _get_lib().mdbx_txn_begin(self._env, parent_ptr, flags, out),
            "mdbx_txn_begin",
        )
        return Txn(self, out[0])

    def close(self) -> None:
        if self._env is not None and self._env != ffi.NULL:
            check(_get_lib().mdbx_env_close(self._env), "mdbx_env_close")
            self._env = None

    def maxvalsize(self) -> int:
        rc = _get_lib().mdbx_env_get_maxvalsize_ex(self._env, DB_DEFAULTS)
        return rc if rc > 0 else 0

    def maxkeysize(self) -> int:
        rc = _get_lib().mdbx_env_get_maxkeysize_ex(self._env)
        return rc if rc > 0 else 0

    def __enter__(self) -> "Env":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class Txn:
    """Обёртка над транзакцией libmdbx (контекстный менеджер)."""

    def __init__(self, env: Env, txn_ptr):
        self.env = env
        self._txn = txn_ptr
        self._done = False

    def commit(self) -> None:
        if not self._done:
            check(_get_lib().mdbx_txn_commit(self._txn), "mdbx_txn_commit")
            self._done = True

    def abort(self) -> None:
        if not self._done:
            _get_lib().mdbx_txn_abort(self._txn)
            self._done = True

    def __enter__(self) -> "Txn":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is None:
            self.commit()
        else:
            self.abort()

    @property
    def ptr(self):
        return self._txn

    def open_dbi(self, name: str, flags: int) -> int:
        dbi = ffi.new("MDBX_dbi *")
        check(_get_lib().mdbx_dbi_open(self.ptr, name.encode(), flags, dbi), "mdbx_dbi_open")
        return int(dbi[0])

    def get(self, dbi: int, key: bytes) -> Tuple[int, Optional[bytes]]:
        kval = _val_from_bytes(key)
        dval = ffi.new("MDBX_val *")
        rc = _get_lib().mdbx_get(self.ptr, dbi, kval.ptr, dval)
        if rc == RC_NOTFOUND:
            return RC_NOTFOUND, None
        check(rc, "mdbx_get")
        return rc, _val_bytes(dval)

    def put(self, dbi: int, key: bytes, value: bytes, flags: int = PUT_UPSERT) -> int:
        kval = _val_from_bytes(key)
        vval = _val_from_bytes(value)
        rc = _get_lib().mdbx_put(self.ptr, dbi, kval.ptr, vval.ptr, flags)
        check(rc, "mdbx_put")
        return rc

    def delete(self, dbi: int, key: bytes, value: Optional[bytes] = None) -> bool:
        kval = _val_from_bytes(key)
        vval = ffi.NULL if value is None else _val_from_bytes(value).ptr
        rc = _get_lib().mdbx_del(self.ptr, dbi, kval.ptr, vval)
        if rc == RC_NOTFOUND:
            return False
        check(rc, "mdbx_del")
        return True

    def cursor(self, dbi: int) -> "Cursor":
        out = ffi.new("void **")
        check(_get_lib().mdbx_cursor_open(self.ptr, dbi, out), "mdbx_cursor_open")
        return Cursor(out[0])

    def count(self, dbi: int) -> int:
        with self.cursor(dbi) as cur:
            return cur.count_all()


class Cursor:
    """Обёртка над курсором libmdbx."""

    def __init__(self, cur_ptr):
        self._cur = cur_ptr

    def get(self, op: int, key: Optional[bytes] = None,
            value: Optional[bytes] = None) -> Tuple[int, Optional[bytes], Optional[bytes]]:
        kval = ffi.new("MDBX_val *")
        dval = ffi.new("MDBX_val *")
        if key is not None:
            kval = _val_from_bytes(key).ptr
        if value is not None:
            dval = _val_from_bytes(value).ptr
        rc = _get_lib().mdbx_cursor_get(self._cur, kval, dval, op)
        if rc in (RC_NOTFOUND, RC_RESULT_TRUE):
            return rc, None, None
        check(rc, "mdbx_cursor_get")
        return rc, _val_bytes(kval), _val_bytes(dval)

    def put(self, key: bytes, value: bytes, flags: int = PUT_UPSERT) -> int:
        kval = _val_from_bytes(key)
        vval = _val_from_bytes(value)
        rc = _get_lib().mdbx_cursor_put(self._cur, kval.ptr, vval.ptr, flags)
        check(rc, "mdbx_cursor_put")
        return rc

    def put_nodupe(self, key: bytes, value: bytes) -> bool:
        """Вставка с уникальностью значения (NODUPDATA). False если уже было."""
        kval = _val_from_bytes(key)
        vval = _val_from_bytes(value)
        rc = _get_lib().mdbx_cursor_put(self._cur, kval.ptr, vval.ptr, PUT_NODUPDATA)
        if rc == RC_KEYEXIST:
            return False
        check(rc, "mdbx_cursor_put")
        return True

    def delete(self, flags: int = PUT_CURRENT) -> None:
        check(_get_lib().mdbx_cursor_del(self._cur, flags), "mdbx_cursor_del")

    def count(self) -> int:
        n = ffi.new("size_t *")
        check(_get_lib().mdbx_cursor_count(self._cur, n), "mdbx_cursor_count")
        return int(n[0])

    def count_all(self) -> int:
        """Количество всех пар в таблице (через обход NEXT)."""
        rc, _, _ = self.get(CURSOR_FIRST)
        n = 0
        while rc == RC_SUCCESS:
            n += 1
            rc, _, _ = self.get(CURSOR_NEXT)
        return n

    def close(self) -> None:
        if self._cur is not None and self._cur != ffi.NULL:
            _get_lib().mdbx_cursor_close(self._cur)
            self._cur = None

    def __enter__(self) -> "Cursor":
        return self

    def __exit__(self, *exc) -> None:
        self.close()