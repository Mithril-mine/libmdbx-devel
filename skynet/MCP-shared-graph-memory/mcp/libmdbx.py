"""Low-level libmdbx bindings (cffi ABI mode).

Все низкоуровневые вызовы собраны здесь; `store.py` использует только этот слой.
Документация: https://libmdbx.dqdkfa.ru/ (C API groups).
"""

from __future__ import annotations

import ctypes
import os
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

    struct MDBX_version_info {
        uint16_t major;
        uint16_t minor;
        uint16_t patch;
        uint16_t tweak;
        const char *semver_prerelease;
        struct {
            const char *datetime;
            const char *tree;
            const char *commit;
            const char *describe;
        } git;
        const char *sourcery;
    };
    struct MDBX_build_info {
        const char *datetime;
        const char *target;
        const char *options;
        const char *compiler;
        const char *flags;
        const char *metadata;
    };
    struct MDBX_canary {
        uint64_t x;
        uint64_t y;
        uint64_t z;
        uint64_t v;
    };

    #define MDBX_CP_DEFAULTS 0
    #define MDBX_CP_COMPACT 1
    #define MDBX_CP_FORCE_DYNAMIC_SIZE 2
    #define MDBX_CP_DONT_FLUSH 4
    #define MDBX_CP_THROTTLE_MVCC 8
    #define MDBX_CP_DISPOSE_TXN 16
    #define MDBX_CP_RENEW_TXN 32
    #define MDBX_CP_OVERWRITE 64

    struct MDBX_commit_latency {
        uint32_t preparation;
        uint32_t gc_wallclock;
        uint32_t audit;
        uint32_t write;
        uint32_t sync;
        uint32_t ending;
        uint32_t whole;
        uint32_t gc_cputime;
        struct {
            uint32_t wloops;
            uint32_t coalescences;
            uint32_t wipes;
            uint32_t flushes;
            uint32_t kicks;
            uint32_t work_counter;
            uint32_t work_rtime_monotonic;
            uint32_t work_xtime_cpu;
            uint32_t work_rsteps;
            uint32_t work_xpages;
            uint32_t work_majflt;
            uint32_t self_counter;
            uint32_t self_rtime_monotonic;
            uint32_t self_xtime_cpu;
            uint32_t self_rsteps;
            uint32_t self_xpages;
            uint32_t self_majflt;
            struct {
                uint32_t time;
                uint64_t volume;
                uint32_t calls;
            } pnl_merge_work, pnl_merge_self;
            uint32_t max_reader_lag;
            uint32_t max_retained_pages;
        } gc_prof;
    };

    struct MDBX_stat {
        uint32_t ms_psize;
        uint32_t ms_depth;
        uint64_t ms_branch_pages;
        uint64_t ms_leaf_pages;
        uint64_t ms_overflow_pages;
        uint64_t ms_entries;
        uint64_t ms_mod_txnid;
    };

    struct MDBX_envinfo {
        struct { uint64_t lower, upper, current, shrink, grow; } mi_geo;
        uint64_t mi_mapsize;
        uint64_t mi_dxb_fsize;
        uint64_t mi_dxb_fallocated;
        uint64_t mi_last_pgno;
        uint64_t mi_recent_txnid;
        uint64_t mi_latter_reader_txnid;
        uint64_t mi_self_latter_reader_txnid;
        uint64_t mi_meta_txnid[3], mi_meta_sign[3];
        uint32_t mi_maxreaders;
        uint32_t mi_numreaders;
        uint32_t mi_dxb_pagesize;
        uint32_t mi_sys_pagesize;
        uint32_t mi_sys_upcblk;
        uint32_t mi_sys_ioblk;
        struct {
            struct { uint64_t x, y; } current, meta[3];
        } mi_bootid;
        uint64_t mi_unsync_volume;
        uint64_t mi_autosync_threshold;
        uint32_t mi_since_sync_seconds16dot16;
        uint32_t mi_autosync_period_seconds16dot16;
        uint32_t mi_since_reader_check_seconds16dot16;
        uint32_t mi_mode;
        struct {
            uint64_t newly, cow, clone, split, merge, spill, unspill;
            uint64_t wops, prefault, mincore, msync, fsync;
        } mi_pgop_stat;
        struct { uint64_t x, y; } mi_dxbid;
    };

    int mdbx_env_create(void **env);
    int mdbx_env_set_option(void *env, int option, uint64_t value);
    int mdbx_env_get_option(const void *env, int option, uint64_t *pvalue);
    int mdbx_env_open(void *env, const char *path, unsigned int flags,
                      unsigned int mode);
    int mdbx_env_openW(void *env, const wchar_t *path, unsigned int flags,
                       unsigned int mode);
    int mdbx_env_close_ex(void *env, bool dont_sync);
    int mdbx_env_sync_ex(void *env, bool force, bool nonblock);
    int mdbx_env_get_flags(const void *env, unsigned int *flags);
    int mdbx_env_set_flags(void *env, unsigned int flags, bool onoff);
    int mdbx_env_info_ex(const void *env, const void *txn, struct MDBX_envinfo *info, size_t bytes);
    int mdbx_env_stat_ex(const void *env, const void *txn, struct MDBX_stat *stat, size_t bytes);
    int mdbx_dbi_stat(const void *txn, unsigned int dbi, struct MDBX_stat *stat, size_t bytes);
    int mdbx_preopen_snapinfo(const char *pathname, struct MDBX_envinfo *info, size_t bytes);
    int mdbx_preopen_snapinfoW(const wchar_t *pathname, struct MDBX_envinfo *info, size_t bytes);
    int mdbx_reader_check(void *env, int *dead);
    int mdbx_env_open_for_recovery(void *env, const char *pathname, unsigned target_meta, bool writeable);
    int mdbx_env_open_for_recoveryW(void *env, const wchar_t *pathname, unsigned target_meta, bool writeable);
    int mdbx_env_turn_for_recovery(void *env, unsigned target_meta);
    int mdbx_env_get_fd(void *env, int *fd);
    int mdbx_txn_begin_ex(void *env, void *parent, unsigned int flags,
                          void **txn, void *context);
    int mdbx_txn_commit_ex(void *txn, struct MDBX_commit_latency *latency);
    int mdbx_txn_abort_ex(void *txn, void *latency);
    uint64_t mdbx_txn_id(const void *txn);
    int mdbx_txn_copy2pathname(void *txn, const char *dest, unsigned int flags);
    int mdbx_txn_copy2pathnameW(void *txn, const wchar_t *dest, unsigned int flags);
    int mdbx_dbi_open(void *txn, const char *name, unsigned int flags,
                      MDBX_dbi *dbi);
    int mdbx_dbi_sequence(void *txn, MDBX_dbi dbi, uint64_t *result,
                          uint64_t increment);
    int mdbx_canary_put(void *txn, struct MDBX_canary *canary);
    int mdbx_canary_get(const void *txn, struct MDBX_canary *canary);
    int mdbx_put(void *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data,
                 unsigned int flags);
    int mdbx_get(void *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data);
    int mdbx_del(void *txn, MDBX_dbi dbi, const MDBX_val *key,
                 const MDBX_val *data);
    int mdbx_cursor_open(void *txn, MDBX_dbi dbi, void **cursor);
    void mdbx_cursor_close(void *cursor);
    int mdbx_cursor_get(void *cursor, MDBX_val *key, MDBX_val *data,
                        int op);
    int mdbx_cursor_put(void *cursor, const MDBX_val *key, MDBX_val *data,
                        unsigned int flags);
    int mdbx_cursor_del(void *cursor, unsigned int flags);
    int mdbx_cursor_count(void *cursor, size_t *count);
    const char *mdbx_strerror(int errnum);
    int mdbx_env_get_maxvalsize_ex(void *env, unsigned int flags);
    int mdbx_env_get_maxkeysize_ex(void *env, unsigned int flags);
    """
)


DEFAULT_SO = "/home/sourcecraft-ci-runner/.local/share/libmdbx-memory/build/libmdbx.so"

_PKG_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.join(_PKG_DIR, "_lib")
_PACKAGE_LIB_NAMES = ("libmdbx.so", "libmdbx.dylib", "libmdbx.dll", "mdbx.dll")


def _find_package_lib():
    """Поиск собранной библиотеки в mcp/_lib (см. tools/build_libmdbx.py)."""
    if not os.path.isdir(_LIB_DIR):
        return None
    for name in _PACKAGE_LIB_NAMES:
        path = os.path.join(_LIB_DIR, name)
        if os.path.isfile(path):
            return path
    return None


def load_library(path: str = None) -> object:
    """Загрузка libmdbx: MDBX_SO_PATH > пакетный _lib > legacy дефолт > системная."""
    global _loaded_path
    if path is None:
        path = (os.environ.get("MDBX_SO_PATH")
                or _find_package_lib()
                or os.path.expanduser(DEFAULT_SO))
    try:
        lib = ffi.dlopen(path)
    except OSError:
        lib = ffi.dlopen("libmdbx.so")
        path = "libmdbx.so"
    _loaded_path = path
    return lib


_lib = None
_loaded_path = None


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

# env_open flags (mdbx.h env_flags)
ENV_RDONLY = 0x20000
ENV_EXCLUSIVE = 0x400000
ENV_ACCEDE = 0x40000000
ENV_WRITEMAP = 0x80000
# sync modes (взаимоисключающие; UTTERLY включает SAFE!)
ENV_DURABLE = 0
ENV_NOMETASYNC = 0x40000
ENV_SAFE_NOSYNC = 0x10000
ENV_UTTERLY_NOSYNC = ENV_SAFE_NOSYNC | 0x100000

SYNC_MODES = {
    "durable": ENV_DURABLE,
    "metasync": ENV_NOMETASYNC,
    "safe_nosync": ENV_SAFE_NOSYNC,
    "utterly_nosync": ENV_UTTERLY_NOSYNC,
}
# безопасная ротация для общего переключения (db_set_mode). UTTERLY_NOSYNC
# исключён: только через отдельный явный инструмент db_enable_utterly_nosync.
SYNC_MODES_SAFE = ("durable", "metasync", "safe_nosync")
_SYNC_BITS = ENV_NOMETASYNC | ENV_UTTERLY_NOSYNC  # покрывает и SAFE

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
RC_WANNA_RECOVERY = -30419
RC_CORRUPTED = -30796

# MDBX_option_t: enum MDBX_option (mdbx.h) — значения по порядку членов.
MDBX_OPT_MAX_DB = 0
MDBX_OPT_MAX_READERS = 1
MDBX_OPT_SYNC_BYTES = 2
MDBX_OPT_SYNC_PERIOD = 3

# Синхронизация: SAFE_NOSYNC — нет fsync на коммите; движок сам сбрасывает
# накопленное по порогам sync_bytes/sync_period (см. mdbx.h sync_modes).
MDBX_SAFE_NOSYNC = 0x10000

# Copy flags для mdbx_txn_copy2pathname / mdbx_env_copy (mdbx.h MDBX_copy_flags).
CP_DEFAULTS = 0
CP_COMPACT = 0x1
CP_FORCE_DYNAMIC_SIZE = 0x2
CP_DONT_FLUSH = 0x4
CP_THROTTLE_MVCC = 0x8
CP_DISPOSE_TXN = 0x10
CP_RENEW_TXN = 0x20
CP_OVERWRITE = 0x40
# набор для «чистых» бэкапов: компактификация + перезапись существующего.
CP_BACKUP = CP_COMPACT | CP_OVERWRITE


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


# --- версия/сборка: mdbx_version и mdbx_build — данные-глобалы -----------------
# cffi (ABI, dlopen) не умеет читать data-символы нецелочисленного типа,
# поэтому структуры читаем через ctypes (импортируется вверху).
class _GitInfo(ctypes.Structure):
    _fields_ = [("datetime", ctypes.c_char_p), ("tree", ctypes.c_char_p),
                ("commit", ctypes.c_char_p), ("describe", ctypes.c_char_p)]


class _VersionInfo(ctypes.Structure):
    _fields_ = [("major", ctypes.c_uint16), ("minor", ctypes.c_uint16),
                ("patch", ctypes.c_uint16), ("tweak", ctypes.c_uint16),
                ("semver_prerelease", ctypes.c_char_p), ("git", _GitInfo),
                ("sourcery", ctypes.c_char_p)]


class _BuildInfo(ctypes.Structure):
    _fields_ = [("datetime", ctypes.c_char_p), ("target", ctypes.c_char_p),
                ("options", ctypes.c_char_p), ("compiler", ctypes.c_char_p),
                ("flags", ctypes.c_char_p), ("metadata", ctypes.c_char_p)]


def _ctypes_lib():
    _get_lib()  # разрешить путь (заполняет _loaded_path)
    return ctypes.CDLL(_loaded_path or "libmdbx.so")


def version_string() -> str:
    """Версия из экспортируемого глобала mdbx_version (надёжно, не inline)."""
    try:
        v = _VersionInfo.in_dll(_ctypes_lib(), "mdbx_version")
        git = v.git.describe or v.semver_prerelease or b""
        return "%u.%u.%u.%u %s" % (v.major, v.minor, v.patch, v.tweak,
                                   git.decode("utf-8", "replace"))
    except Exception:
        return "?"


def build_string() -> str:
    """Опции сборки из экспортируемого глобала mdbx_build."""
    try:
        b = _BuildInfo.in_dll(_ctypes_lib(), "mdbx_build")
        opts = b.options or b""
        return opts.decode("utf-8", "replace")
    except Exception:
        return ""


def preopen_snapinfo(path: str) -> dict:
    """Базовая информация о БД БЕЗ открытия env (mdbx_preopen_snapinfo).

    Заполняет ТОЛЬКО поля, читаемые без mmap и блокировок: pagesize,
    геометрию, last_pgno, **последний txnid (mi_recent_txnid)** и bootid
    текущей мета-страницы. Критично: переживает пересоздание LCK-файла.
    На Windows используется *W-вариант.

    Для полных meta_txnid[3] нужен Env.diag() после read-only открытия.
    """
    info = ffi.new("struct MDBX_envinfo *")
    if os.name == "nt":
        rc = _get_lib().mdbx_preopen_snapinfoW(
            ctypes.c_wchar_p(path), info, ffi.sizeof("struct MDBX_envinfo"))
    else:
        rc = _get_lib().mdbx_preopen_snapinfo(
            path.encode(), info, ffi.sizeof("struct MDBX_envinfo"))
    check(rc, "mdbx_preopen_snapinfo")
    return {
        "geo": {"lower": int(info.mi_geo.lower),
                "upper": int(info.mi_geo.upper),
                "current": int(info.mi_geo.current),
                "shrink": int(info.mi_geo.shrink),
                "grow": int(info.mi_geo.grow)},
        "last_pgno": int(info.mi_last_pgno),
        "recent_txnid": int(info.mi_recent_txnid),
        "dxb_pagesize": int(info.mi_dxb_pagesize),
        "sys_pagesize": int(info.mi_sys_pagesize),
        "bootid_current": {"x": int(info.mi_bootid.current.x),
                           "y": int(info.mi_bootid.current.y)},
        "bootid_meta": [{"x": int(m.x), "y": int(m.y)}
                        for m in info.mi_bootid.meta],
    }


def check(rc: int, where: str = "") -> int:
    """Проверяет код возврата: 0 и MDBX_RESULT_TRUE(-1) — не ошибки."""
    if rc == RC_SUCCESS or rc == RC_RESULT_TRUE:
        return rc
    raise LibmdbxError(rc, where)


def open_for_recovery_probe(path: str, target_meta: int = 0) -> dict:
    """Read-only диагностика мета-страницы для recovery (mdbx_chk-механика).

    Открывает отдельный env на конкретной мете (writeable=False), читает
    ключевые поля и закрывает. НИЧЕГО не модифицирует — это информационный
    probe; ремонт остаётся за mdbx_chk / восстановлением из бэкапа.
    """
    if not 0 <= target_meta <= 2:
        raise ValueError("target_meta must be 0..2")
    env = ffi.new("void **")
    check(_get_lib().mdbx_env_create(env), "mdbx_env_create")
    if os.name == "nt":
        rc = _get_lib().mdbx_env_open_for_recoveryW(
            env[0], ctypes.c_wchar_p(path), target_meta, False)
    else:
        rc = _get_lib().mdbx_env_open_for_recovery(
            env[0], path.encode(), target_meta, False)
    if rc != RC_SUCCESS:
        _get_lib().mdbx_env_close_ex(env[0], True)
        check(rc, "mdbx_env_open_for_recovery")
    try:
        info = ffi.new("struct MDBX_envinfo *")
        rc = _get_lib().mdbx_env_info_ex(
            env[0], ffi.NULL, info, ffi.sizeof("struct MDBX_envinfo"))
        if rc != RC_SUCCESS:
            return {"error": "mdbx_env_info_ex(%s): %s" % (rc, strerror(rc))}
        return {
            "target_meta": target_meta,
            "meta_txnid": [int(x) for x in info.mi_meta_txnid],
            "recent_txnid": int(info.mi_recent_txnid),
            "geo_current": int(info.mi_geo.current),
            "dxb_pagesize": int(info.mi_dxb_pagesize),
            "bootid_current": {"x": int(info.mi_bootid.current.x),
                               "y": int(info.mi_bootid.current.y)},
            "bootid_meta": [{"x": int(m.x), "y": int(m.y)}
                            for m in info.mi_bootid.meta],
        }
    finally:
        _get_lib().mdbx_env_close_ex(env[0], True)


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

    _IS_WINDOWS = os.name == "nt"

    def __init__(self, path: str, maxdbs: int = 32, create: bool = True,
                 sync_bytes: int = 64 << 20, sync_period: int = 60,
                 readonly: bool = False, exclusive: bool = False,
                 accede: bool = False, writemap: bool = False,
                 sync_mode: str = "safe_nosync", sync_flags: int = None):
        self.path = path
        self.flags = NOSUBDIR
        if readonly:
            self.flags |= ENV_RDONLY
        if exclusive:
            self.flags |= ENV_EXCLUSIVE
        if accede:
            self.flags |= ENV_ACCEDE
        if writemap:
            self.flags |= ENV_WRITEMAP
        if sync_flags is not None:
            # явный набор sync-битов (для тестов и переключения)
            self.flags |= sync_flags
        elif sync_mode in SYNC_MODES:
            self.flags |= SYNC_MODES[sync_mode]
        else:
            raise ValueError("unknown sync_mode %r" % sync_mode)
        out = ffi.new("void **")
        check(_get_lib().mdbx_env_create(out), "mdbx_env_create")
        self._env = out[0]
        check(_get_lib().mdbx_env_set_option(self._env, MDBX_OPT_MAX_DB, maxdbs),
              "mdbx_env_set_option(max_db)")
        # mode=0 означает "открыть существующее, не создавать" (mdbx.h env_open).
        mode = 0o644 if create else 0
        rc = self._env_open(path, mode)
        check(rc, "mdbx_env_open")
        # sync_bytes/sync_period в read-only режиме менять нельзя
        # (движок вернёт EACCES) — пропускаем установку.
        if not readonly:
            if sync_bytes:
                check(_get_lib().mdbx_env_set_option(self._env, MDBX_OPT_SYNC_BYTES,
                                                     sync_bytes),
                      "mdbx_env_set_option(sync_bytes)")
            if sync_period:
                check(_get_lib().mdbx_env_set_option(self._env, MDBX_OPT_SYNC_PERIOD,
                                                     sync_period),
                      "mdbx_env_set_option(sync_period)")

    def _env_open(self, path: str, mode: int) -> int:
        """Открытие env; на Windows — wchar-вариант (mdbx_env_openW)."""
        if self._IS_WINDOWS:
            return _get_lib().mdbx_env_openW(
                self._env, ctypes.c_wchar_p(path), self.flags, mode)
        return _get_lib().mdbx_env_open(self._env, path.encode(), self.flags, mode)

    # --- интроспекция ----------------------------------------------------------
    def diag(self) -> dict:
        """Диагностика по мета-страницам и bootid (живут в файле БД, НЕ в LCK).

        Возвращает txnid всех трёх мета-страниц, bootid (current+meta),
        recent/latter txnid, геометрию и режим. Критерий «достигли ли данные
        диска»: сравнение mi_recent_txnid с max(mi_meta_txnid) и bootid.
        """
        info = ffi.new("struct MDBX_envinfo *")
        rc = _get_lib().mdbx_env_info_ex(self._env, ffi.NULL, info,
                                         ffi.sizeof("struct MDBX_envinfo"))
        check(rc, "mdbx_env_info_ex")
        return {
            "geo": {"lower": int(info.mi_geo.lower),
                    "upper": int(info.mi_geo.upper),
                    "current": int(info.mi_geo.current),
                    "shrink": int(info.mi_geo.shrink),
                    "grow": int(info.mi_geo.grow)},
            "mapsize": int(info.mi_mapsize),
            "dxb_fsize": int(info.mi_dxb_fsize),
            "dxb_fallocated": int(info.mi_dxb_fallocated),
            "last_pgno": int(info.mi_last_pgno),
            "recent_txnid": int(info.mi_recent_txnid),
            "latter_reader_txnid": int(info.mi_latter_reader_txnid),
            "self_latter_reader_txnid": int(info.mi_self_latter_reader_txnid),
            "meta_txnid": [int(x) for x in info.mi_meta_txnid],
            "meta_sign": [int(x) for x in info.mi_meta_sign],
            "maxreaders": int(info.mi_maxreaders),
            "numreaders": int(info.mi_numreaders),
            "dxb_pagesize": int(info.mi_dxb_pagesize),
            "sys_pagesize": int(info.mi_sys_pagesize),
            "sys_upcblk": int(info.mi_sys_upcblk),
            "sys_ioblk": int(info.mi_sys_ioblk),
            "bootid_current": {"x": int(info.mi_bootid.current.x),
                               "y": int(info.mi_bootid.current.y)},
            "bootid_meta": [{"x": int(m.x), "y": int(m.y)}
                            for m in info.mi_bootid.meta],
            "unsync_volume": int(info.mi_unsync_volume),
            "autosync_threshold": int(info.mi_autosync_threshold),
            "since_sync_16dot16": int(info.mi_since_sync_seconds16dot16),
            "autosync_period_16dot16": int(info.mi_autosync_period_seconds16dot16),
            "since_reader_check_16dot16": int(info.mi_since_reader_check_seconds16dot16),
            "mode": int(info.mi_mode),
            "dxbid": {"x": int(info.mi_dxbid.x), "y": int(info.mi_dxbid.y)},
            "pgop_stat": {"newly": int(info.mi_pgop_stat.newly),
                          "cow": int(info.mi_pgop_stat.cow),
                          "clone": int(info.mi_pgop_stat.clone),
                          "split": int(info.mi_pgop_stat.split),
                          "merge": int(info.mi_pgop_stat.merge),
                          "spill": int(info.mi_pgop_stat.spill),
                          "unspill": int(info.mi_pgop_stat.unspill),
                          "wops": int(info.mi_pgop_stat.wops),
                          "prefault": int(info.mi_pgop_stat.prefault),
                          "mincore": int(info.mi_pgop_stat.mincore),
                          "msync": int(info.mi_pgop_stat.msync),
                          "fsync": int(info.mi_pgop_stat.fsync)},
        }

    def stat(self, dbi: int = None) -> dict:
        """Статистика env (или конкретной таблицы при dbi в активной txn)."""
        st = ffi.new("struct MDBX_stat *")
        rc = _get_lib().mdbx_env_stat_ex(self._env, ffi.NULL, st,
                                         ffi.sizeof("struct MDBX_stat"))
        check(rc, "mdbx_env_stat_ex")
        out = {"psize": int(st.ms_psize), "depth": int(st.ms_depth),
               "branch_pages": int(st.ms_branch_pages),
               "leaf_pages": int(st.ms_leaf_pages),
               "overflow_pages": int(st.ms_overflow_pages),
               "entries": int(st.ms_entries),
               "mod_txnid": int(st.ms_mod_txnid)}
        return out

    def get_option(self, option: int) -> int:
        val = ffi.new("uint64_t *")
        check(_get_lib().mdbx_env_get_option(self._env, option, val),
              "mdbx_env_get_option(%d)" % option)
        return int(val[0])

    def get_flags(self) -> int:
        val = ffi.new("unsigned int *")
        check(_get_lib().mdbx_env_get_flags(self._env, val), "mdbx_env_get_flags")
        return int(val[0])

    def set_flags(self, flags: int, onoff: bool) -> None:
        check(_get_lib().mdbx_env_set_flags(self._env, flags, bool(onoff)),
              "mdbx_env_set_flags")

    def set_sync_mode(self, mode: str) -> None:
        """Переключение sync-режима на лету (mdbx_env_set_flags).

        Снимает все sync-биты, затем ставит нужный. UTTERLY_NOSYNC включает
        SAFE_NOSYNC внутри себя, поэтому снятие идёт полным набором.
        """
        if mode not in SYNC_MODES:
            raise ValueError("unknown sync_mode %r" % mode)
        self.set_flags(_SYNC_BITS, False)
        bits = SYNC_MODES[mode]
        if bits:
            self.set_flags(bits, True)

    def reader_check(self) -> int:
        dead = ffi.new("int *")
        rc = _get_lib().mdbx_reader_check(self._env, dead)
        if rc not in (RC_SUCCESS, RC_RESULT_TRUE):
            check(rc, "mdbx_reader_check")
        return int(dead[0])

    def get_fd(self) -> int:
        """Файловый дескриптор файла БД (mdbx_env_get_fd).

        Позволяет делать fstat/linkat для детекции удаления файла и
        LIFELINE-ссылки (сторонняя процедура; движок сам по нему не пишет).
        На Windows fd — HANDLE (механизм st_nlink там не используется).
        """
        fd = ffi.new("int *")
        check(_get_lib().mdbx_env_get_fd(self._env, fd), "mdbx_env_get_fd")
        return int(fd[0])

    def file_stat(self) -> dict:
        """fstat(fd) файла БД: nlink/size/ino/dev/mtime (+ признаки удаления).

        st_nlink — ключ детекции удаления: у «живого» файла >= 1, при unlink
        становится 0. LIFELINE-ссылка делает инвариант >= 2.
        """
        import stat as _stat

        st = os.fstat(self.get_fd())
        return {
            "nlink": st.st_nlink,
            "size": st.st_size,
            "ino": st.st_ino,
            "dev": st.st_dev,
            "mtime": int(st.st_mtime),
            "mode": _stat.S_IFMT(st.st_mode),
            "deleted": st.st_nlink == 0,
        }

    def sync(self, force: bool = False, nonblock: bool = True) -> None:
        """Сброс буферов данных на диск (mdbx_env_sync_ex).

        force=True — принудительный сброс; force=False — polling: сброс только
        если достигнут порог sync_bytes/sync_period. nonblock=True не ждёт
        чужую write-txn (вернёт MDBX_BUSY)."""
        rc = _get_lib().mdbx_env_sync_ex(self._env, force, nonblock)
        if rc not in (RC_SUCCESS, RC_RESULT_TRUE, RC_BUSY):
            check(rc, "mdbx_env_sync_ex")

    def begin(self, readonly: bool = False, parent: Optional[object] = None) -> "Txn":
        out = ffi.new("void **")
        flags = TXN_RDONLY if readonly else TXN_READWRITE
        parent_ptr = ffi.NULL if parent is None else parent._txn
        check(
            _get_lib().mdbx_txn_begin_ex(self._env, parent_ptr, flags, out, ffi.NULL),
            "mdbx_txn_begin_ex",
        )
        return Txn(self, out[0])

    def close(self) -> None:
        if self._env is not None and self._env != ffi.NULL:
            check(_get_lib().mdbx_env_close_ex(self._env, False), "mdbx_env_close_ex")
            self._env = None

    def maxvalsize(self) -> int:
        rc = _get_lib().mdbx_env_get_maxvalsize_ex(self._env, DB_DEFAULTS)
        return rc if rc > 0 else 0

    def maxkeysize(self) -> int:
        rc = _get_lib().mdbx_env_get_maxkeysize_ex(self._env, DB_DEFAULTS)
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
        # опционально: Store-владелец для сбора commit-latency (см. _begin_write)
        self._store = None
        self.last_latency = None

    def commit(self, latency: Optional[dict] = None) -> Optional[dict]:
        """Коммит; при latency не-None возвращает словарь MDBX_commit_latency.

        Стадии (в 1/65536 с): preparation/gc_wallclock/audit/write/sync/ending/
        whole/gc_cputime. gc_prof заполняется только в сборках
        с MDBX_ENABLE_PROFGC — здесь он просто ноль/недоступен.
        """
        if self._done:
            return None
        lat = ffi.new("struct MDBX_commit_latency *")
        check(_get_lib().mdbx_txn_commit_ex(self._txn, lat),
              "mdbx_txn_commit_ex")
        self._done = True
        out = {
            "preparation": int(lat.preparation),
            "gc_wallclock": int(lat.gc_wallclock),
            "audit": int(lat.audit),
            "write": int(lat.write),
            "sync": int(lat.sync),
            "ending": int(lat.ending),
            "whole": int(lat.whole),
            "gc_cputime": int(lat.gc_cputime),
        }
        gp = lat.gc_prof
        out["gc_prof"] = {
            "wloops": int(gp.wloops), "coalescences": int(gp.coalescences),
            "wipes": int(gp.wipes), "flushes": int(gp.flushes),
            "kicks": int(gp.kicks),
            "work_counter": int(gp.work_counter),
            "work_rtime_monotonic": int(gp.work_rtime_monotonic),
            "work_xtime_cpu": int(gp.work_xtime_cpu),
            "work_rsteps": int(gp.work_rsteps),
            "work_xpages": int(gp.work_xpages),
            "work_majflt": int(gp.work_majflt),
            "self_counter": int(gp.self_counter),
            "self_rtime_monotonic": int(gp.self_rtime_monotonic),
            "self_xtime_cpu": int(gp.self_xtime_cpu),
            "self_rsteps": int(gp.self_rsteps),
            "self_xpages": int(gp.self_xpages),
            "self_majflt": int(gp.self_majflt),
            "pnl_merge_work": {"time": int(gp.pnl_merge_work.time),
                               "volume": int(gp.pnl_merge_work.volume),
                               "calls": int(gp.pnl_merge_work.calls)},
            "pnl_merge_self": {"time": int(gp.pnl_merge_self.time),
                               "volume": int(gp.pnl_merge_self.volume),
                               "calls": int(gp.pnl_merge_self.calls)},
            "max_reader_lag": int(gp.max_reader_lag),
            "max_retained_pages": int(gp.max_retained_pages),
        }
        self.last_latency = out
        if self._store is not None:
            self._store._note_commit_latency(out)
        return out if latency is not None else None

    def id(self) -> int:
        """Идентификатор транзакции (mdbx_txn_id); 0 для неактивной."""
        if self._done:
            return 0
        return int(_get_lib().mdbx_txn_id(self._txn))

    def copy2pathname(self, dest: str, flags: int = CP_BACKUP) -> dict:
        """Консистентная копия БД в файл (mdbx_txn_copy2pathname).

        Копия делается из read-txn (MVCC-снапшот) — безопасна при живых
        писателях. По умолчанию COMPACT|OVERWRITE (компактификация).
        На Windows используется *W-вариант.
        """
        if self._done:
            raise RuntimeError("txn already finished")
        if os.name == "nt":
            rc = _get_lib().mdbx_txn_copy2pathnameW(
                self._txn, ctypes.c_wchar_p(dest), flags)
        else:
            rc = _get_lib().mdbx_txn_copy2pathname(self._txn, dest.encode(), flags)
        check(rc, "mdbx_txn_copy2pathname")
        return {"dest": dest, "txnid": self.id()}

    def abort(self) -> None:
        if not self._done:
            check(_get_lib().mdbx_txn_abort_ex(self._txn, ffi.NULL),
                  "mdbx_txn_abort_ex")
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

    def dbi_stat(self, dbi: int) -> dict:
        """Статистика таблицы (mdbx_dbi_stat) в рамках активной txn."""
        st = ffi.new("struct MDBX_stat *")
        check(_get_lib().mdbx_dbi_stat(self.ptr, dbi, st,
                                       ffi.sizeof("struct MDBX_stat")),
              "mdbx_dbi_stat")
        return {"psize": int(st.ms_psize), "depth": int(st.ms_depth),
                "branch_pages": int(st.ms_branch_pages),
                "leaf_pages": int(st.ms_leaf_pages),
                "overflow_pages": int(st.ms_overflow_pages),
                "entries": int(st.ms_entries),
                "mod_txnid": int(st.ms_mod_txnid)}

    def sequence(self, dbi: int, increment: int = 1) -> int:
        """mdbx_dbi_sequence: атомарный инкремент счётчика таблицы.

        Возвращает текущее значение ДО изменения (как в C API).
        В read-only транзакции increment должен быть 0.
        """
        out = ffi.new("uint64_t *")
        rc = _get_lib().mdbx_dbi_sequence(self.ptr, dbi, out, increment)
        if rc == RC_RESULT_TRUE:
            # переполнение
            raise LibmdbxError(rc, "mdbx_dbi_sequence overflow")
        check(rc, "mdbx_dbi_sequence")
        return int(out[0])

    def canary_get(self) -> dict:
        """mdbx_canary_get: четыре uint64 маркера (x,y,z,v)."""
        can = ffi.new("struct MDBX_canary *")
        check(_get_lib().mdbx_canary_get(self.ptr, can), "mdbx_canary_get")
        return {"x": int(can.x), "y": int(can.y), "z": int(can.z),
                "v": int(can.v)}

    def canary_put(self, x=None, y=None, z=None) -> None:
        """mdbx_canary_put: обновляет x/y/z; v всегда = номер транзакции."""
        can = ffi.new("struct MDBX_canary *")
        can.x = x or 0
        can.y = y or 0
        can.z = z or 0
        check(_get_lib().mdbx_canary_put(self.ptr, can), "mdbx_canary_put")


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