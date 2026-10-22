"""Store — персистентная память роя на libmdbx.

Схема (docs/SCHEMA.md):
  records   DEFAULTS                       key=канон.строка → JSON body
  vocab     DEFAULTS                       key=module:{m} | {m}:{topic} → {}
  history   DEFAULTS                       key=_history:{key}:{ts} → JSON
  ids       DEFAULTS                       key=record_key → uint64 id
  id2key    INTEGERKEY                     key=uint64 id → record_key
  inverted  DUPSORT|DUPFIXED|INTEGERDUP    key=term → set uint64 id
  links     DUPSORT|DUPFIXED|INTEGERDUP    key=subj_id\\0pred → set obj_id
  simhash   INTEGERKEY                     key=uint64 id → uint64 hash
  access    INTEGERKEY                     key=uint64 id → 24B (score,last_access,count)
  archive   DEFAULTS                       key=record_key → JSON (gc-миграция)
  meta      DEFAULTS                       key=b"next_id" → uint64

Контракт: одна write-транзакция на операцию (в т.ч. LRU-касания — батчем);
env открыт с MDBX_SAFE_NOSYNC, фоновый тред раз в секунду шлёт sync_poll,
пороги сброса — sync_bytes/sync_period (см. ARCHITECTURE.md §«Синхронизация»).
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import math
import os
import re
import shutil
import sys
import threading
import time
from collections import deque

from . import index as idx
from . import libmdbx as mdbx
from .errors import (
    MemoryError,
    busy_io,
    db_file_deleted,
    invalid,
    readonly_mode,
    schema_outdated,
    schema_too_new,
    size_limit,
)
from .normalize import RECORD_TYPES, Normalizer

ID_LEN = 8
ACCESS_LEN = 24  # float64 score + uint64 last_access + uint64 count

BUSY_RETRIES = 3
GC_HOT = 0.7
GC_WARM = 0.3
LRU_RATE_LIMIT_SECONDS = 60
SYNC_POLL_INTERVAL_SECONDS = 1.0
# D3/N11: периодическая чистка мёртвых reader-слотов (kill -9 агентов роя
# оставляет их занятыми; без чистки maxreaders может исчерпаться).
READER_CHECK_INTERVAL_SECONDS = 60.0

# --- слой ветки процесса (роевая изоляция, N1): per-process override поверх
# глобальной meta.branch_current. Агент роя выставляет свою ветку один раз —
# и его вызовы с layer="" не переключаются чужим branch_activate.
ENV_LAYER = "SHARED_GRAPH_MEMORY_LAYER"

# --- резервное копирование и защита от удаления файла БД ----------------------
ENV_BACKUP_DIR = "SHARED_GRAPH_MEMORY_BACKUP_DIR"
ENV_AUTO_BACKUP_SECONDS = "SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS"
ENV_PROMPT_BACKUP_SECONDS = "SHARED_GRAPH_MEMORY_PROMPT_BACKUP_SECONDS"
ENV_BACKUP_KEEP = "SHARED_GRAPH_MEMORY_BACKUP_KEEP"
ENV_BACKUP_MAX_MB = "SHARED_GRAPH_MEMORY_BACKUP_MAX_MB"
DEFAULT_BACKUP_KEEP = 7
DEFAULT_BACKUP_MAX_MB = 512
BACKUP_STATE_FILE = ".backup-state.json"
BACKUP_LIFELINE = "current.mdbx"
COMMIT_LATENCY_WINDOW = 16


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, "") or default)
    except ValueError:
        return default


def _pack_u64(n: int) -> bytes:
    return int(n).to_bytes(ID_LEN, sys.byteorder, signed=False)


def _unpack_u64(b: bytes) -> int:
    return int.from_bytes(b, sys.byteorder, signed=False)


def _now() -> int:
    return int(time.time())


# Версия схемы хранилища. Инкремент при ОБРАТНО-несовместимых изменениях
# форматов таблиц/ключей; при аддитивных расширениях не меняется.
SCHEMA_VERSION = "2"

# Реестр зондов (SystemMap): допустимые значения kind/status.
# kind: usdt/bookmark — наблюдение; injected — fault-инъекция;
# dev_assert — встроенная проверка; collect/watch — из MPROBE_* макросов.
PROBE_KINDS = ("usdt", "bookmark", "injected", "dev_assert", "collect", "watch")
# status: жизненный цикл зонда по probe-bus.md §4 (реестр).
PROBE_STATUSES = ("candidate", "inserted", "validated")
# Статусы портирования C→C++ (поле cpp_status тела символа карты):
# none — не начат; mapped — есть C++-каркас/декларация; partial — частично
# переписан; done — эквивалент подтверждён (cpp_eq). map_port_status и
# map_put_symbol оперируют этим набором (N6: валидация на входе).
CPP_STATUSES = ("none", "mapped", "partial", "done")
# Версия модуля (дублирует mcp.__version__; здесь — без import-циклов).
MODULE_VERSION = "0.2.0"

# Регулярка версионного slug базового слоя ("0.15", "0.16.0").
_VVER_RE = re.compile(r"^\d+(\.\d+){1,2}$")

# Признак ключа чужого feature-слоя в перегенерируемых таблицах:
# версионный префикс слоя ("0.16.abc.N/") перед fn:/type:/macro:/region:...
_LAYER_KEY_RE = re.compile(r"^\d+(\.\d+)+\.[A-Za-z0-9._-]+\.\d+/")


@contextlib.contextmanager
def _file_lock(path: str):
    """Системно-независимая файловая блокировка (stdlib, без зависимостей).

    Адvisory-лок на отдельный lock-файл `<path>.lock` рядом с целевым:
    захватываем, yield, освобождаем. POSIX — fcntl.flock, Windows —
    msvcrt.locking (замок на диапазон файла). Защищает read-modify-write
    `.backup-state.json` от параллельных процессов роя (ревью #164-r5,
    R1): без неё last-writer-wins затирает персистентный auto_skipped_limit.
    """
    lock_path = path + ".lock"
    try:
        with open(lock_path, "a+b") as fh:
            if os.name == "nt":
                import msvcrt  # локально: недоступен на POSIX
                msvcrt.locking(fh.fileno(), msvcrt.LK_LOCK, 1)
            else:
                import fcntl  # локально: недоступен на Windows
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                if os.name == "nt":
                    import msvcrt
                    fh.seek(0)
                    msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
    except OSError as e:
        # блокировка — вспомогательная: сбой не должен валить бэкап
        sys.stderr.write("state lock: %s\n" % e)
        yield


class Store:
    """Основной объект: открывает env, управляет таблицами."""

    TABLES = {
        "records": mdbx.DB_DEFAULTS,
        "vocab": mdbx.DB_DEFAULTS,
        "history": mdbx.DB_DEFAULTS,
        "ids": mdbx.DB_DEFAULTS,
        "id2key": mdbx.INTEGERKEY,
        "inverted": mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP,
        "links": mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP,
        "simhash": mdbx.INTEGERKEY,
        "access": mdbx.INTEGERKEY,
        "archive": mdbx.DB_DEFAULTS,
        "meta": mdbx.DB_DEFAULTS,
        "symbols": mdbx.DB_DEFAULTS,
        "call_edges": mdbx.DUPSORT,
        "call_edges_rev": mdbx.DUPSORT,
        "groups": mdbx.DUPSORT,
        "sym_ids": mdbx.DB_DEFAULTS,
        "sym_id2key": mdbx.INTEGERKEY,
        "sym_aliases": mdbx.DB_DEFAULTS,
        "regions": mdbx.DB_DEFAULTS,   # #if-дерево: region:{module}:{n} -> JSON
        "uncovered": mdbx.DB_DEFAULTS,  # uncovered-острова: uncovered:{module}:{n}
        "links_in": mdbx.INTEGERKEY,   # денормализованный счётчик входящих рёбер
        # --- слой тестов/CI (зарезервирован в SCHEMA §1.1, наполняется
        # коллекторами из ctest/CI-артефактов) ---
        "coverage": mdbx.DB_DEFAULTS,    # test_id -> JSON {symbols:[fn:...], pct}
        "test_metrics": mdbx.DB_DEFAULTS,  # тест-кейс -> JSON окно истории
        "ci_jobs": mdbx.DB_DEFAULTS,     # {workflow}/{job} -> JSON
        "covered_by": mdbx.DB_DEFAULTS,  # covered_by:{fn:...} -> JSON [test_id]
        "coverage_blocks": mdbx.DB_DEFAULTS,  # coverage_blocks:{test}:{sym} -> JSON [blocks]
        # --- слой карты проекта (весь репозиторий, вне C/C++ AST) ---
        "project_files": mdbx.DB_DEFAULTS,  # rel_path -> JSON {kind, lang, module, deps}
        "file_deps": mdbx.DB_DEFAULTS,   # file:rel -> JSON [dep_rel]
        # --- реестр probes/SystemMap ---
        "probes": mdbx.DB_DEFAULTS,      # probe:{tag} -> JSON {kind, mechanism, symbol, test_theme, status}
        # --- наблюдаемые вызовы (runtime traces) ---
        "call_traces": mdbx.DB_DEFAULTS,   # caller -> JSON {callee: count}
        "call_traces_rev": mdbx.DUPSORT,  # callee -> caller (обратные рёбра)
        # --- ADR (архитектурные решения) ---
        "adr": mdbx.DB_DEFAULTS,         # adr:{name} -> JSON (context/decision/consequences)
        # --- мост с codebase-memory-mcp (опциональный) ---
        "bridge": mdbx.DB_DEFAULTS,      # bridge:{our_key} -> JSON {cbm_project, cbm_node, mapped_at}
        # --- слои веток (нелинейное версионирование БЗ) ---
        "branches": mdbx.DB_DEFAULTS,    # branch:{layer} -> JSON атрибутов слоя
    }

    def __init__(self, path: str, maxdbs: int = 64, max_value_bytes: int = 16 << 20,
                 sync_mode: str = "safe_nosync", readonly: bool = False,
                 _skip_sync_thread: bool = False):
        self.path = path
        self.max_value_bytes = max_value_bytes
        self.sync_mode = sync_mode
        self._readonly = readonly
        # per-process слой (env SHARED_GRAPH_MEMORY_LAYER): читается один раз —
        # детерминирован на время жизни Store и не перетирается чужими
        # branch_activate из параллельных процессов роя (N1).
        self._env_layer = (os.environ.get(ENV_LAYER) or "").strip().strip("/")
        # read-only открытие НЕ должно трогать LCK на запись: используем
        # ACCEDE чтобы присоединиться к уже открытой БД без конфликта флагов.
        self.env = mdbx.Env(path, maxdbs=maxdbs, readonly=readonly,
                            accede=readonly, sync_mode=sync_mode)
        self.norm = Normalizer()
        self._dbi = {}
        # открываем все таблицы один раз (CREATE) в первой write-txn;
        # в read-only режиме CREATE не нужен — dbi открываются с DEFAULTS.
        # ВАЖНО: при readonly существующие БД могут НЕ содержать новые таблицы
        # (напр. branches/coverage_blocks) — открываем с NOSUBDIR-толерантностью:
        # отсутствующая таблица -> None в _dbi (методы вернут пусто), а полный
        # upgrade выполняется Store.upgrade() при rw-старте сервера.
        with self.env.begin(readonly=readonly) as txn:
            for name in self.TABLES:
                fl = self.TABLES[name] | (mdbx.CREATE if not readonly else 0)
                try:
                    self._dbi[name] = txn.open_dbi(name, fl)
                except mdbx.LibmdbxError as e:
                    # несовместимые флаги (таблица из более старой схемы,
                    # например call_traces было DUPSORT, теперь DEFAULTS):
                    # не фейлим — помечаем None, upgrade() пересоздаст.
                    # readonly: отсутствующая таблица тоже → None.
                    if ("INCOMPATIBLE" in str(e)) or \
                       (readonly and "NOTFOUND" in str(e)):
                        self._dbi[name] = None
                    else:
                        raise
        # F5.2/R2: БД, созданная более новой версией модуля — явный отказ
        # (иначе читали бы/писали неизвестную схему; см. _check_schema_too_new)
        self._check_schema_too_new()
        self._load_vocab()
        self.next_id = self._load_next_id()
        self.last_gc = None
        self._closed = False
        # LRU-касания: in-memory rate-limit (60с на запись, per-process)
        # и буфер, который сбрасывается одной write-txn (см. _flush_touches).
        self._lru_last: dict = {}
        self._touch_ids: list = []
        # commit-latency последних write-транзакций (кольцо).
        self._latencies = deque(maxlen=COMMIT_LATENCY_WINDOW)
        # наблюдаемость writer-контеншена роя (D4/N12, per-process):
        self._busy_retries = 0     # сколько раз BUSY заставил повторить begin
        self._busy_failures = 0    # сколько раз исчерпались BUSY_RETRIES
        # --- резервное копирование / защита от удаления ----------------------
        self.backup_dir = self._resolve_backup_dir(path)
        self.auto_backup_seconds = _env_int(ENV_AUTO_BACKUP_SECONDS, 0)
        self.prompt_backup_seconds = _env_int(ENV_PROMPT_BACKUP_SECONDS, 0)
        self.backup_keep = max(1, _env_int(ENV_BACKUP_KEEP, DEFAULT_BACKUP_KEEP))
        self.backup_max_bytes = max(1, _env_int(ENV_BACKUP_MAX_MB,
                                                DEFAULT_BACKUP_MAX_MB)) << 20
        self.db_file_deleted = False
        self.backup_notice = None   # одноразовое уведомление агенту
        self.backup_pending = False  # PROMPT-интервал: спросить пользователя
        self._lifeline_error = None
        self._lifeline_mode = None   # "link" | "copy" | None (Windows)
        if not readonly:
            self._protect_lifeline()
        # Фоновый тред: раз в секунду mdbx_env_sync_poll (дешёвый nonblocking
        # сброс по порогам sync_bytes/sync_period) + контроль st_nlink.
        # Транзакций не создаёт. В read-only режиме тред не запускаем
        # (синк не нужен и может писать в LCK).
        self._sync_stop = threading.Event()
        self._sync_thread = None
        self._last_reader_check = 0.0  # D3: таймер чистки reader-слотов
        if not readonly and not _skip_sync_thread:
            self._sync_thread = threading.Thread(
                target=self._sync_loop, name="store-sync-poll", daemon=True)
            self._sync_thread.start()

    # --- sync ---------------------------------------------------------------
    def _sync_loop(self) -> None:
        while not self._sync_stop.wait(SYNC_POLL_INTERVAL_SECONDS):
            try:
                self._check_deleted()
                self.env.sync(force=False, nonblock=True)
                # D3/N11: раз в READER_CHECK_INTERVAL секунд чистим мёртвые
                # reader-слоты (kill -9 агентов роя оставляет их занятыми).
                now = time.time()
                if (now - self._last_reader_check
                        >= READER_CHECK_INTERVAL_SECONDS):
                    self._last_reader_check = now
                    self.env.reader_check()
            except Exception:
                pass  # опрос синка не должен ломать сервер

    def _stop_sync_thread(self) -> None:
        self._sync_stop.set()
        if self._sync_thread is not None and self._sync_thread.is_alive():
            self._sync_thread.join(timeout=2.0)

    # --- диагностика и режимы --------------------------------------------------
    def diag(self) -> dict:
        """Диагностика БД: мета-страницы/txnid/bootid (живут в файле, не в LCK).

        Критерий «достигли ли данные диска»:
          - env.meta_txnid — txnid всех трёх мета-страниц (всегда доступен
            через открытый env);
          - env.bootid_current vs bootid_meta — «разошлась» ли БД с boot`ом
            (при отличии — при следующем открытии откат к steady);
          - preopen.recent_txnid — последний txnid видимый БЕЗ открытия; этот
            вызов валиден только при закрытой env или синхронизированной
            (при живой nosync-env вернёт MDBX_CORRUPTED) — обрабатываем
            best-effort, для живого сервера основой служит env.diag().
        """
        out = {}
        try:
            out["env"] = self.env.diag()
        except Exception as e:  # noqa: BLE001
            out["env"] = {"error": str(e)}
        try:
            out["preopen"] = mdbx.preopen_snapinfo(self.path)
        except Exception as e:  # noqa: BLE001
            out["preopen"] = {"error": str(e),
                              "note": "вален при закрытой/синхронизированной env"}
        # D4/N12: наблюдаемость writer-контеншена роя. Без этих метрик узкое
        # место single-writer невидимо, пока не упрётся в busy_io у агентов.
        lat = self.commit_latency()
        wholes = sorted(lat_rec.get("whole") for lat_rec in self._latencies
                        if lat_rec.get("whole") is not None)
        p90_us = 0.0
        if wholes:
            # наивный percentile: floor-индекс int(n*0.9), кламп к последнему
            # элементу. Осознанно: на малых окнах (n<=10) смещается к max —
            # консервативно для мониторинга; это не off-by-one.
            p90_us = round(wholes[min(len(wholes) - 1,
                                      int(len(wholes) * 0.9))]
                           * 1000000.0 / 65536.0, 1)
        out["ops"] = {
            "busy_retries": self._busy_retries,
            "busy_failures": self._busy_failures,
            "commits_in_window": len(self._latencies),
            "commit_p90_us": p90_us,
            "commit_mean_us": lat["window_whole_us"]["mean"],
            "commit_max_us": lat["window_whole_us"]["max"],
        }
        return out

    def flush_sync(self) -> dict:
        """Принудительный сброс на диск (mdbx_env_sync force=True)."""
        if self._readonly:
            raise readonly_mode(
                "хранилище открыто в read-only режиме",
                "вызовите db_set_mode(readonly=false) для переключения")
        self.env.sync(force=True, nonblock=False)
        return {"flushed": True}

    def set_sync_mode(self, mode: str) -> dict:
        """Переключение sync-режима на лету (durable|metasync|safe_nosync).

        UTTERLY_NOSYNC сюда НЕ входит: он снимает гарантии durable и доступен
        только через отдельный опасный инструмент enable_utterly_nosync.
        """
        if mode not in mdbx.SYNC_MODES_SAFE:
            raise invalid("неизвестный sync_mode %r" % mode,
                          "используйте durable|metasync|safe_nosync")
        if self._readonly:
            raise invalid("read-only режим не позволяет менять sync_mode",
                          "сначала переключитесь в read-write")
        self.env.set_sync_mode(mode)
        self.sync_mode = mode
        return {"sync_mode": mode}

    def enable_utterly_nosync(self) -> dict:
        """ОПАСНО: полное отключение синхронизации (безопасно только для
        одноразовых кэшей). Данные после краха процесса могут пропасть.

        Вызывается ТОЛЬКО осознанно через отдельный MCP-инструмент
        db_enable_utterly_nosync, а не через ротацию db_set_mode.
        """
        if self._readonly:
            raise invalid("read-only режим не позволяет менять sync_mode",
                          "сначала переключитесь в read-write")
        self.env.set_sync_mode("utterly_nosync")
        self.sync_mode = "utterly_nosync"
        return {"sync_mode": "utterly_nosync", "danger": True,
                "warn": "гарантии durable отключены; после краха данные "
                        "последних транзакций могут быть потеряны"}

    def set_readonly(self, readonly: bool) -> dict:
        """Переоткрытие env в read-only/read-write режиме.

        RDONLY задаётся только при mdbx_env_open, поэтому требуется закрыть
        и заново открыть среду. Активных транзакций/курсоров быть не должно.
        """
        if readonly == self._readonly:
            return {"readonly": readonly}
        with self._guard_no_active() as _:
            pass
        self._reopen(readonly)
        return {"readonly": readonly}

    def db_stat(self, table: str = None) -> dict:
        """Статистика env или одной таблицы."""
        if table is None:
            return self.env.stat()
        if table not in self.TABLES:
            raise invalid("неизвестная таблица %r" % table,
                          "список таблиц: %s" % ", ".join(sorted(self.TABLES)))
        with self.env.begin(readonly=True) as txn:
            return txn.dbi_stat(self.dbi(txn, table))

    def readers(self, check: bool = False) -> dict:
        """Число активных читателей; check=True — вычистить мёртвые."""
        dead = self.env.reader_check() if check else 0
        try:
            n = self.env.diag().get("numreaders", 0)
        except Exception:  # noqa: BLE001
            n = 0
        return {"numreaders": n, "dead_cleared": dead}

    # --- защита от удаления файла БД (не-Windows) --------------------------
    def _resolve_backup_dir(self, path: str) -> str:
        """Каталог бэкапов: env или `<каталог_бд>/backups` (та же ФС!)."""
        env = os.environ.get(ENV_BACKUP_DIR)
        if env:
            return os.path.abspath(env)
        return os.path.join(os.path.dirname(os.path.abspath(path)), "backups")

    def _link_or_copy(self, dest: str) -> str:
        """Защитная ссылка на файл БД; возвращает режим "link" или "copy".

        Порядок попыток:
          1. os.link(self.path, dest) — обычная жёсткая ссылка по пути
             (работает на macOS/Linux/Windows при одной ФС);
          2. os.link через /proc/self/fd|/dev/fd — если путь уже недоступен;
          3. полная копия файла — на кросс-ФС (EXDEV: tmpfs vs основной диск,
             GitHub Actions) и там, где fd-link запрещён (macOS EPERM).
        Копия сохраняет данные и детекцию удаления (путь пропал + nlink < 2),
        хотя и не даёт инвариант st_nlink >= 2.
        """
        try:
            os.link(self.path, dest)
            return "link"
        except OSError as e:
            sys.stderr.write("cbm link: %s\n" % e)
        fd = self.env.get_fd()
        for proc in ("/proc/self/fd/%d" % fd, "/dev/fd/%d" % fd):
            try:
                os.link(proc, dest)
                return "link"
            except OSError:
                continue
        with os.fdopen(os.dup(fd), "rb") as src, open(dest, "wb") as dst:
            src.seek(0)  # mdbx оставляет fd-офсет где угодно — читаем с начала
            shutil.copyfileobj(src, dst, 1024 * 1024)
        return "copy"

    def _protect_lifeline(self) -> None:
        """Инвариант защиты: LIFELINE-ссылка `current.mdbx` в backup_dir.

        Если файл БД удалят из-под процесса, останется ссылка/копия на тот же
        контент, а nlink == 1 станет сигналом детекции (вместе с пропажей пути).
        """
        if os.name == "nt":
            return
        try:
            os.makedirs(self.backup_dir, exist_ok=True)
            if self.env.file_stat()["nlink"] < 2:
                link = os.path.join(self.backup_dir, BACKUP_LIFELINE)
                if not os.path.lexists(link):
                    self._lifeline_mode = self._link_or_copy(link)
            self._lifeline_error = None
        except OSError as e:
            self._lifeline_error = str(e)
            self.backup_notice = "LIFELINE backup link unavailable: %s" % e

    def _emergency_snapshot(self) -> "str|None":
        """Аварийный снапшот при детекции удаления файла БД."""
        try:
            txnid = 0
            try:
                with self.env.begin(readonly=True) as txn:
                    txnid = txn.id()
            except Exception:  # noqa: BLE001
                pass
            name = "snapshot_%s_txnid%d.db" % (time.strftime("%y%m%d_%H%M%S"),
                                               txnid)
            dest = os.path.join(self.backup_dir, name)
            if not os.path.lexists(dest):
                self._link_or_copy(dest)
            self.backup_notice = ("файл БД был удалён из-под процесса; "
                                  "аварийный снапшот: %s" % dest)
            return dest
        except OSError as e:
            self.backup_notice = "emergency snapshot failed: %s" % e
            return None

    def _check_deleted(self) -> None:
        """fstat(fd): если путь БД пропал и nlink < 2 — файл удалён.

        Тогда: аварийный снапшот (жёсткая ссылка) + флаг-предохранитель.
        Порог 2, т.к. после _protect_lifeline у живого файла nlink >= 2;
        дополнительно проверяется существование пути, чтобы не срабатывать
        на здоровом файле без LIFELINE (например, кросс-ФС).
        """
        if os.name == "nt" or self.db_file_deleted:
            return
        try:
            st = self.env.file_stat()
            if st["nlink"] >= 2 or os.path.exists(self.path):
                return
        except Exception:  # noqa: BLE001
            return  # fstat при переоткрытии — не ломаем сервер
        self._emergency_snapshot()
        self.db_file_deleted = True

    def _raise_if_deleted(self) -> None:
        if self.db_file_deleted:
            raise db_file_deleted(
                "файл БД был удалён из-под процесса; аварийный снапшот сохранён",
                "восстановите файл из backup_dir (os.link) или остановите сервер")

    def before_call(self) -> dict:
        """Хук перед каждым вызовом инструмента: детекция удаления + таймеры.

        Возвращает одноразовый notice для агента (пропуск авто-бэкапа,
        PROMPT-запрос). При удалённом файле БД — поднимает DB_FILE_DELETED.
        """
        self._check_deleted()
        self._raise_if_deleted()
        self.maybe_run_auto_backup()
        out = {}
        notice = self.backup_notice
        self.backup_notice = None
        if self.backup_pending:
            out["backup_pending"] = True
            out["backup_prompt"] = (
                "пора сделать резервную копию; спросите пользователя: "
                "db_backup (сделать точку сохранения) или "
                "db_backup skip=true (пропустить)")
        if notice:
            out["backup_notice"] = notice
        return out

    def fileinfo(self) -> dict:
        """fstat(fd) файла БД + признаки удаления (для db_fileinfo)."""
        fs = self.env.file_stat()
        fs["path"] = self.path
        fs["path_exists"] = os.path.exists(self.path)
        fs["db_file_deleted"] = self.db_file_deleted
        fs["lifeline"] = os.path.join(self.backup_dir, BACKUP_LIFELINE)
        fs["lifeline_ok"] = os.path.isfile(fs["lifeline"])
        fs["lifeline_error"] = self._lifeline_error
        fs["lifeline_mode"] = self._lifeline_mode
        return fs

    def commit_latency(self) -> dict:
        """Стадии последних write-коммитов (1/65536 c → µs) + сводка."""
        us = lambda x: round(x * 1000000.0 / 65536.0, 1)  # noqa: E731
        last = None
        if self._latencies:
            last = {k: us(v) for k, v in self._latencies[-1].items()
                    if isinstance(v, int)}
        wholes = [lat["whole"] for lat in self._latencies
                  if lat.get("whole") is not None]
        mean_us = max_us = 0.0
        if wholes:
            scale = 1000000.0 / 65536.0
            mean_us = round(sum(wholes) * scale / len(wholes), 1)
            max_us = round(max(wholes) * scale, 1)
        return {
            "count": len(self._latencies),
            "last_us": last,
            "window_whole_us": {"mean": mean_us, "max": max_us},
            "gc_prof_available": bool(self._latencies
                                      and self._latencies[-1].get("gc_prof")),
        }

    def _note_commit_latency(self, lat: dict) -> None:
        self._latencies.append(lat)

# --- резервное копирование ----------------------------------------------
    def _state_path(self) -> str:
        return os.path.join(self.backup_dir, BACKUP_STATE_FILE)

    def _load_backup_state(self) -> dict:
        try:
            with open(self._state_path(), encoding="utf-8") as fh:
                return json.load(fh)
        except Exception:  # noqa: BLE001
            return {}

    # ключи state-файла, которые процессы обновляют (атомарно, под блокировкой);
    # merge-семантика: НЕ трогаем ключи, которых нет в st (могли быть обновлены
    # другим процессом роя между нашим чтением и записью).
    _STATE_MERGE_KEYS = ("last_auto", "last_auto_txnid", "auto_skipped_limit",
                         "last_prompt")

    def _save_backup_state(self, st: dict) -> None:
        """Атомарная запись state-файла под блокировкой, с merge-семантикой.

        Read-modify-write всего файла при 20 процессах — last-writer-wins:
        процесс, сохраняющий свой last_auto, затирал бы auto_skipped_limit,
        выставленный другим, — персистентная видимость флага рою (D2) теряется
        тем же механизмом. Поэтому пишем ТОЛЬКО свои ключи поверх текущего
        содержимого (read-current под блокировкой → merge → запись).
        """
        try:
            os.makedirs(self.backup_dir, exist_ok=True)
            with _file_lock(self._state_path()):
                cur = self._load_backup_state()
                for k in self._STATE_MERGE_KEYS:
                    if k in st:
                        if st[k] is None:
                            # явный сигнал снятия (напр. auto_skipped_limit):
                            # удаляем ключ, иначе stale True пережил бы merge
                            cur.pop(k, None)
                        else:
                            cur[k] = st[k]
                tmp = self._state_path() + ".tmp"
                with open(tmp, "w", encoding="utf-8") as fh:
                    json.dump(cur, fh)
                os.replace(tmp, self._state_path())
        except OSError as e:
            sys.stderr.write("state save: %s\n" % e)

    def _list_backups(self, prefix: str = "") -> list:
        out = []
        try:
            for fn in sorted(os.listdir(self.backup_dir)):
                if (fn.startswith(prefix) and fn.endswith(".mdbx")
                        and fn != BACKUP_LIFELINE):
                    p = os.path.join(self.backup_dir, fn)
                    out.append({"name": fn, "path": p,
                                "size": os.path.getsize(p)})
        except OSError as e:
            sys.stderr.write("list backups: %s\n" % e)
        return out

    def _backup_dir_size(self) -> int:
        """Суммарный размер независимых копий (без жёстких ссылок).

        Исключения (не расходуют бюджет, см. ревью #164):
        - LIFELINE `current.mdbx` — жёсткая ссылка на живую БД (0 места);
          исключена поимённо. ВНИМАНИЕ: в copy-режиме (кросс-ФС, macOS
          EPERM) это полноценная копия — тогда лимит занижается на размер
          БД; случай редкий, учтён как MINOR.
        - EMERGENCY `snapshot_*.db` — тоже жёсткая ссылка, но исключена
          ПОБОЧНО через суффикс `.db` (фильтр `.mdbx` ниже), не поимённо;
          переименование snapshot в `.mdbx` молча изменит семантику лимита.
        """
        total = 0
        try:
            for fn in os.listdir(self.backup_dir):
                if fn.endswith(".mdbx") and fn != BACKUP_LIFELINE:
                    total += os.path.getsize(os.path.join(self.backup_dir, fn))
        except OSError as e:
            sys.stderr.write("backup dir size: %s\n" % e)
        return total

    def _rotation_candidates(self, keep: int = None) -> list:
        """Старейшие AUTO-копии сверх лимита — кандидаты на удаление."""
        keep = keep or self.backup_keep
        files = self._list_backups("auto-")
        if len(files) <= keep:
            return []
        return files[:-keep]  # sorted по имени = по времени

    def _prune_auto(self) -> int:
        """Авто-ротация AUTO-копий сверх backup_keep (после успешного бэкапа).

        Удаляет старейшие, чтобы каталог не рос бесконечно (в т.ч. в /tmp).
        """
        removed = 0
        for c in self._rotation_candidates():
            try:
                os.unlink(c["path"])
                removed += 1
            except OSError:
                continue  # файл мог исчезнуть (другой процесс) — пропускаем
        return removed

    def backup(self, kind: str = "pin", dest: str = None,
               force: bool = False) -> dict:
        """Внутренняя точка сохранения (MVCC-снапшот) через txn_copy2pathname.

        kind: 'auto' — тихий интервал (таймер maybe_run_auto_backup,
        ротируется _prune_auto); 'pin' — закреплённая точка сохранения по
        запросу пользователя / PROMPT-подтверждению: ВНУТРИ backup-каталога,
        учитывается в лимите и tiers, НИКОГДА не ротируется.
        Имя: {kind}-YYYYMMDD-HHMMSS_txnid<N>.mdbx.
        Флаги: COMPACT | OVERWRITE (компактификация).

        Экспорт данных наружу (внешняя архивация / перенос на другой хост) —
        отдельный метод `export(dest)`: вне backup-каталога, без учёта в
        лимите и tiers, не ротируется. Через MCP kind не передаётся:
        db_backup = pin, db_export = export. kind="auto" НЕДОСТУПЕН через
        MCP (там kind вообще убран), но Store.backup(kind="auto") остаётся
        публичным для Python-API и используется таймером
        maybe_run_auto_backup — прямой вызов auto подчиняется лимиту, но
        НЕ ротируется _prune_auto до следующего тика таймера (ротация
        привязана к таймеру).

        Лимит каталога (D2/N10): раньше backup_max_bytes проверялся только
        для auto (и молча — notice терялся в памяти процесса). Теперь:
        - auto — пропуск с ПЕРСИСТЕНТНЫМ флагом в state-файле (виден всем
          процессам роя и переживает рестарт, снимается cleanup/успехом);
        - pin — явная контрактная ошибка (агент должен увидеть причину, а не
          тихий пропуск), обход — force=True.
        """
        if self.db_file_deleted:
            self._raise_if_deleted()
        if kind not in ("auto", "pin"):
            raise invalid("неизвестный kind бэкапа %r" % kind,
                          "используйте auto|pin (экспорт — export())")
        os.makedirs(self.backup_dir, exist_ok=True)
        # проверка лимита по умолчанию каталога — только когда dest внутри
        # backup_dir (копия во внешний путь лимит не расходует — но извне
        # это уже export()). Прямые вызовы pin/auto подчиняются бюджету:
        # без этой защиты каталог раздувался бы мимо ротации _prune_auto.
        # Штатный auto из maybe_run_auto_backup проверяет лимит ДО вызова,
        # поэтому для него это условие не срабатывает.
        in_dir = dest is None or os.path.dirname(os.path.abspath(dest)) == \
            os.path.abspath(self.backup_dir)
        if in_dir and not force \
                and self._backup_dir_size() > self.backup_max_bytes:
            raise invalid(
                "каталог бэкапов (%.1f МБ) превышает лимит %d МБ"
                % (self._backup_dir_size() / (1 << 20),
                   self.backup_max_bytes >> 20),
                "освободите место (db_cleanup confirm=true / удалите старые "
                "копии), поднимьте SHARED_GRAPH_MEMORY_BACKUP_MAX_MB или "
                "повторите с force=true")
        with self.env.begin(readonly=True) as txn:
            txnid = txn.id()
            if dest is None:
                name = "%s-%s_txnid%d.mdbx" % (
                    kind, time.strftime("%Y%m%d-%H%M%S"), txnid)
                dest = os.path.join(self.backup_dir, name)
            r = txn.copy2pathname(dest, mdbx.CP_BACKUP)
        return {"path": r["dest"], "txnid": r["txnid"], "kind": kind,
                "size": os.path.getsize(r["dest"])}

    def export(self, dest: str) -> dict:
        """Экспорт данных НАРУЖУ: копия для внешней архивации/переноса.

        Семантика (по решению владельца): db_export — это формирование
        копии для ВНЕШНЕГО потребления, поэтому:
        - `dest` ОБЯЗАТЕЛЕН и должен указывать ВНЕ backup-каталога
          (иначе это не экспорт, а внутренняя точка — db_backup);
          «вне» включает ПОДКАТАЛОГИ backup_dir (ревью #164): иначе
          `backup_dir/sub/x.mdbx` прошёл бы проверку (dirname ≠ backup_dir)
          и ускользнул из лимита/tiers (listdir нерекурсивен) — учётная дыра;
          сравнение — через realpath+normcase (_is_inside_backup_dir), что
          закрывает symlink-обход и Windows-регистр (ревью #164-r4);
        - копия НЕ учитывается в лимите каталога (не расходует бюджет)
          и НЕ попадает в tiers backup_status;
        - НЕ ротируется (судьбу копии решает владелец пути назначения).
        Флаги те же: COMPACT | OVERWRITE (компактификация).
        """
        if self.db_file_deleted:
            self._raise_if_deleted()
        if not dest or not str(dest).strip():
            raise invalid("export: dest обязателен",
                          "укажите путь назначения ВНЕ backup-каталога "
                          "(внешняя архивация / перенос на другой хост)")
        dest = str(dest).strip()
        if self._is_inside_backup_dir(dest):
            raise invalid(
                "export: dest %r внутри backup-каталога" % dest,
                "экспорт — наружу каталога (включая подкаталоги); для "
                "внутренней точки сохранения используйте db_backup (pin)")
        d = os.path.dirname(os.path.abspath(dest))
        if d:
            os.makedirs(d, exist_ok=True)
        with self.env.begin(readonly=True) as txn:
            r = txn.copy2pathname(dest, mdbx.CP_BACKUP)
        return {"path": r["dest"], "txnid": r["txnid"], "kind": "export",
                "size": os.path.getsize(r["dest"])}

    def _is_inside_backup_dir(self, dest: str) -> bool:
        """dest внутри backup_dir (включая подкаталоги) — сравнение путей.

        realpath резолвит симлинки и «..» (иначе symlink внутрь каталога
        обошёл бы проверку); normcase casefold'ит на Windows (ФС там
        регистронезависима: `c:\\bk\\sub` vs `C:\\Bk` без него — outside).
        На POSIX обе нормализации no-op. commonpath бросает ValueError на
        разных дисках Windows — там пути заведомо вне каталога, False.
        """
        adir = os.path.normcase(os.path.realpath(dest))
        abk = os.path.normcase(os.path.realpath(self.backup_dir))
        try:
            return adir == abk or os.path.commonpath([adir, abk]) == abk
        except ValueError:
            return False

    def _current_txnid(self) -> int:
        """Текущий txnid (хвост мета) — без записи."""
        try:
            mt = self.env.diag().get("meta_txnid") or []
            if isinstance(mt, (list, tuple)) and mt:
                return int(mt[-1])
            return 0
        except Exception:  # noqa: BLE001
            return 0

    def maybe_run_auto_backup(self) -> None:
        """Таймеры AUTO (тихо) и PROMPT (с запросом); 0 = выключено.

        AUTO-бэкап создаётся ТОЛЬКО если БД реально изменялась с прошлого
        (txnid вырос): пустые «бэкапы без оснований» и мусорные каталоги в
        /tmp исключаются. Пропуск по лимиту каталога — ПЕРСИСТЕНТНЫЙ флаг
        `auto_skipped_limit` в state-файле (D2/N10): виден всем процессам
        роя и переживает рестарт; notice одноразовый (in-memory) дополняет
        его для текущего процесса. Флаг снимается успешным auto-бэкапом.
        """
        if os.name == "nt" or self.db_file_deleted:
            return
        now = time.time()
        st = self._load_backup_state()
        if (self.auto_backup_seconds > 0
                and now - st.get("last_auto", 0) >= self.auto_backup_seconds):
            cur = self._current_txnid()
            last = st.get("last_auto_txnid", 0)
            if cur > last:
                # БД реально изменялась — только тогда бэкап
                if self._backup_dir_size() > self.backup_max_bytes:
                    self.backup_notice = (
                        "auto-backup пропущен: каталог бэкапов превышает лимит "
                        "%d МБ" % (self.backup_max_bytes >> 20))
                    # пропуск по лимиту — осознанное решение: таймер
                    # обновляем (следующий бэкап не раньше интервала),
                    # флаг показывает рою состояние лимита
                    st["last_auto"] = now
                    st["last_auto_txnid"] = cur
                    if not st.get("auto_skipped_limit"):
                        st["auto_skipped_limit"] = True
                    self._save_backup_state(st)
                else:
                    try:
                        self.backup(kind="auto")
                        self._prune_auto()
                        st["last_auto"] = now
                        st["last_auto_txnid"] = cur
                        # снимаем флаг через None (не pop): merge-запись
                        # _save_backup_state видит ключ и удаляет его из файла
                        if st.get("auto_skipped_limit"):
                            st["auto_skipped_limit"] = None
                        self._save_backup_state(st)
                    except Exception as e:  # noqa: BLE001
                        self.backup_notice = "auto-backup failed: %s" % e
                        # TOCTOU (ревью #164): между нашей проверкой и
                        # backup() другой процесс мог заполнить каталог —
                        # тогда персистентный флаг обязан быть выставлен и
                        # здесь, иначе рой/рестарт не видят, что auto-бэкапы
                        # фактически блокированы лимитом.
                        # НО (ревью #164-r4, 1b): флаг ставим ТОЛЬКО если
                        # причина падения — именно лимит (контрактный
                        # INVALID из backup()). Не-лимитные ошибки (диск,
                        # DB_FILE_DELETED) на переполненном каталоге не
                        # должны ложно сигнализировать «auto блокирован
                        # лимитом» — иначе после исправления первопричины
                        # auto не возобновился бы, а рой видел бы ложный флаг.
                        # Таймер НЕ обновляем (ревью #164-r5, R1): иначе при
                        # постоянной не-лимитной ошибке (диск полон) auto
                        # молча затихал бы на полный интервал; следующий
                        # такт снова попробует.
                        limit_hit = (isinstance(e, MemoryError)
                                     and getattr(e, "code", None) == "INVALID"
                                     and self._backup_dir_size()
                                     > self.backup_max_bytes)
                        if limit_hit:
                            st["auto_skipped_limit"] = True
                            # провал по лимиту тоже сдвигает таймер
                            st["last_auto"] = now
                            st["last_auto_txnid"] = cur
                            self._save_backup_state(st)
            # иначе: БД не менялась — тихий пропуск, ничего не пишем
        if (self.prompt_backup_seconds > 0 and not self.backup_pending
                and now - st.get("last_prompt", 0) >= self.prompt_backup_seconds):
            self.backup_pending = True

    def run_prompt_backup(self, approve: bool, force: bool = False) -> dict:
        """Ответ агента на PROMPT-запрос: сделать/пропустить копию."""
        if not self.backup_pending:
            return {"pending": False}
        self.backup_pending = False
        st = self._load_backup_state()
        result = {"pending": False, "approved": approve}
        if approve:
            try:
                r = self.backup(kind="pin", force=force)
                result.update(r)
            except Exception as e:  # noqa: BLE001
                result["error"] = str(e)
        st["last_prompt"] = time.time()
        self._save_backup_state(st)
        return result

    def cleanup(self, dry_run: bool = True, confirm: bool = False,
                keep: int = None) -> dict:
        """Ротация: показать/удалить кандидатов (только тир AUTO)."""
        cands = self._rotation_candidates(keep)
        names = [c["name"] for c in cands]
        total = sum(c["size"] for c in cands)
        if dry_run or not confirm:
            return {"dry_run": True, "removed": [], "candidates": names,
                    "total_bytes": total, "keep": keep or self.backup_keep}
        removed = []
        for c in cands:
            try:
                os.unlink(c["path"])
                removed.append(c["name"])
            except OSError:
                continue
        # Флаг auto_skipped_limit отражает СОСТОЯНИЕ лимита («места нет»),
        # а не факт удаления auto-копий (ревью #164-#9): при pin-only
        # переполнении ротация ничего не удаляет, но оператор мог освободить
        # место вручную (rm pin-*/другое) — перепроверяем размер каталога и
        # снимаем флаг, как только место есть. Иначе рой навсегда остался бы
        # с флагом «auto блокирован» без штатного пути разблокировки.
        st = self._load_backup_state()
        if st.get("auto_skipped_limit") and \
                self._backup_dir_size() <= self.backup_max_bytes:
            # None, не pop — merge-запись удалит ключ из файла
            st["auto_skipped_limit"] = None
            self._save_backup_state(st)
        return {"dry_run": False, "removed": removed, "candidates": names,
                "total_bytes": total, "keep": keep or self.backup_keep}

    def recover(self, target_meta: int = 0) -> dict:
        """Диагностика меты для аварийного восстановления (БЕЗопасная).

        open_for_recovery требует исключительного доступа, поэтому на живом
        сервере возвращает троицу мет + рекомендацию: остановить сервер и
        использовать mdbx_chk либо восстановить из db_backup."""
        if not 0 <= target_meta <= 2:
            raise invalid("target_meta вне диапазона", "используйте 0..2")
        try:
            probe = mdbx.open_for_recovery_probe(self.path, target_meta)
            probe["note"] = ("мета-страница %d прочитана (эксклюзивный доступ); "
                             "для ремонта используйте mdbx_chk или "
                             "восстановление из db_backup" % target_meta)
            return probe
        except mdbx.LibmdbxError as e:
            # ожидаемо: env уже открыт этим процессом (MDBX_BUSY)
            diag = {}
            try:
                diag = self.env.diag()
            except Exception:  # noqa: BLE001
                pass
            return {
                "target_meta": target_meta,
                "meta_txnid": diag.get("meta_txnid"),
                "busy": "open_for_recovery требует остановленного сервера",
                "note": ("переключение меты выполняется только при остановленном "
                         "сервере (mdbx_chk) либо восстановлением из db_backup"),
                "detail": str(e),
            }

    def backup_status(self) -> dict:
        """Состояние бэкапов: каталог, таймеры, счётчики тиров, кандидаты."""
        st = self._load_backup_state()
        next_auto = (st.get("last_auto", 0) + self.auto_backup_seconds
                     if self.auto_backup_seconds else None)
        next_prompt = (st.get("last_prompt", 0) + self.prompt_backup_seconds
                       if self.prompt_backup_seconds else None)
        return {
            "backup_dir": self.backup_dir,
            "auto_backup_seconds": self.auto_backup_seconds,
            "prompt_backup_seconds": self.prompt_backup_seconds,
            "backup_keep": self.backup_keep,
            "backup_max_mb": self.backup_max_bytes >> 20,
            "dir_size_mb": round(self._backup_dir_size() / (1 << 20), 2),
            "last_auto": st.get("last_auto"),
            "last_prompt": st.get("last_prompt"),
            "next_auto": next_auto,
            "next_prompt": next_prompt,
            "pending": self.backup_pending,
            "auto_skipped_limit": bool(st.get("auto_skipped_limit")),
            "db_file_deleted": self.db_file_deleted,
            "notice": self.backup_notice,
            "lifeline_error": self._lifeline_error,
            "tiers": {
                "lifeline": os.path.isfile(os.path.join(self.backup_dir,
                                                        BACKUP_LIFELINE)),
                "auto": len(self._list_backups("auto-")),
                "pin": len(self._list_backups("pin-")),
                "snapshot": len([f for f in self._list_backups("snapshot_")]),
            },
            "rotation_candidates": [c["name"]
                                    for c in self._rotation_candidates()],
        }

    # --- guard/reopen (внутренние) -------------------------------------------
    def _guard_no_active(self):
        """Контекст-менеджер: убедиться, что нет активных write-txn/курсоров.

        В текущей модели все операции используют короткие `with`-транзакции
        внутри Store, поэтому активных транзакций вне вызовов не бывает.
        Оставлен как точка расширения для будущего stateful-режима.
        """
        class _G:
            def __enter__(self):
                return None

            def __exit__(self, *exc):
                return False
        return _G()

    def _reopen(self, readonly: bool) -> None:
        """Закрыть и переоткрыть env (синхронно, без потери данных)."""
        self._stop_sync_thread()
        try:
            self.env.sync(force=True, nonblock=False)
        except Exception as e:
            sys.stderr.write("env sync: %s\n" % e)
        self.env.close()
        self._readonly = readonly
        self.env = mdbx.Env(self.path, maxdbs=64, readonly=readonly,
                            accede=readonly, sync_mode=self.sync_mode)
        self._dbi = {}
        with self.env.begin(readonly=readonly) as txn:
            for name in self.TABLES:
                fl = self.TABLES[name] | (mdbx.CREATE if not readonly else 0)
                try:
                    self._dbi[name] = txn.open_dbi(name, fl)
                except mdbx.LibmdbxError as e:
                    # несовместимые/отсутствующие таблицы → None; при переходе
                    # в rw upgrade() ниже создаст/пересоздаст их
                    if ("INCOMPATIBLE" in str(e)) or \
                       (readonly and "NOTFOUND" in str(e)):
                        self._dbi[name] = None
                    else:
                        raise
        if not readonly:
            # переход rw: миграция схемы (новые таблицы, INCOMPATIBLE-пересоздание,
            # backfill covered_by) — до начала обслуживания записей
            self.upgrade()
        if not readonly:
            self._protect_lifeline()
        self._load_vocab()  # словарь мог измениться внешними процессами
        self._sync_stop = threading.Event()
        self._sync_thread = None
        if not readonly:
            self._sync_thread = threading.Thread(
                target=self._sync_loop, name="store-sync-poll", daemon=True)
            self._sync_thread.start()

    # --- init -----------------------------------------------------------------
    def _load_vocab(self) -> None:
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "vocab")
            if dbi is None:
                return  # очень старая БД без vocab — словарь остаётся пустым
            cur = txn.cursor(dbi)
            rc, key, _ = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                text = key.decode("utf-8", "replace")
                if text.startswith("module:"):
                    self.norm.add_module(text[7:])
                elif ":" in text:
                    _m, _, _t = text.partition(":")
                    self.norm.add_topic(_t)
                rc, key, _ = cur.get(mdbx.CURSOR_NEXT)

    def _load_next_id(self) -> int:
        try:
            with self.env.begin() as txn:
                meta = self.dbi(txn, "meta")
                rc, val = txn.get(meta, b"next_id")
                if val is None:
                    # первое создание БД: фиксируем версию схемы
                    txn.put(meta, b"schema_ver", SCHEMA_VERSION.encode())
                    txn.put(meta, b"next_id", _pack_u64(1))
                    return 1
                return _unpack_u64(val)
        except Exception:  # noqa: BLE001  (read-only env: не пишем)
            with self.env.begin(readonly=True) as txn:
                rc, val = txn.get(self.dbi(txn, "meta"), b"next_id")
                return _unpack_u64(val) if val is not None else 1

    def _alloc_id_locked(self, txn) -> int:
        """Атомарное выделение id из meta.next_id ВНУТРИ текущей write-txn.

        Единственный способ получить новый числовой id (записи, символы):
        read-modify-write счётчика в той же транзакции, что и запись.
        single-writer движка + RMW в txn делают аллокацию корректной и для
        нескольких процессов на одной БД (роевая модель) — в отличие от
        кэша self.next_id, который разъезжается при конкурентных писателях.
        """
        meta = self.dbi(txn, "meta")
        rc, val = txn.get(meta, b"next_id")
        nid = _unpack_u64(val) if val is not None and len(val) == ID_LEN else 1
        txn.put(meta, b"next_id", _pack_u64(nid + 1))
        self.next_id = nid + 1  # display-кэш (лог старта, stats)
        return nid

    def dbi(self, txn, name: str) -> int:
        return self._dbi[name]

    def _opt_dbi(self, txn, name: str):
        """dbi для опциональных таблиц (None-handle в старой/readonly БД)."""
        return self._dbi.get(name)

    def _schema_ver_from_meta(self) -> str:
        """schema_ver из meta (read-only); при ошибке — SCHEMA_VERSION."""
        try:
            with self.env.begin(readonly=True) as txn:
                rc, vmeta = txn.get(self.dbi(txn, "meta"), b"schema_ver")
                if vmeta is not None:
                    return vmeta.decode()
        except Exception:  # noqa: BLE001
            pass
        return SCHEMA_VERSION

    def _check_schema_too_new(self) -> None:
        """Отклонить БД, созданную более НОВОЙ версией модуля.

        Сценарий (ревью #164-r5, R2): устаревший MCP-модуль на свежей БД.
        Мы знаем СВОЮ схему, но не знаем чужую более новую — продолжать
        работать значило бы читать/писать таблицы с неизвестной семантикой.
        Контрактный SCHEMA_TOO_NEW вместо undefined behavior.
        """
        disk_ver = self._schema_ver_from_meta()
        try:
            if int(disk_ver) > int(SCHEMA_VERSION):
                raise schema_too_new(
                    "БД создана более новой версией модуля: schema_ver %s > "
                    "поддерживаемая %s" % (disk_ver, SCHEMA_VERSION),
                    "обновите MCP-модуль до версии, поддерживающей схему %s"
                    % disk_ver)
        except ValueError:
            # schema_ver не число — чужая/битая мета: тоже несовместима
            if disk_ver != SCHEMA_VERSION:
                raise schema_too_new(
                    "неизвестный schema_ver %r в мета БД" % disk_ver,
                    "обновите MCP-модуль или пересоздайте БД")

    def _check_opt_table(self, name: str) -> None:
        """Методы опциональных таблиц: None-handle на старой схеме — ошибка.

        Различие (ревью #164-r5, R2): None-handle при актуальной схеме —
        это легитимный read-only старт (таблица не создана, читается пустой);
        None-handle при старой схеме (schema_ver < SCHEMA_VERSION) — реальная
        несовместимость, которую агент должен увидеть и устранить переходом
        в rw-режим (db_set_mode readonly=false → upgrade), а НЕ молчаливый
        пустой результат (неотличимый от «данных нет»).
        """
        if self._dbi.get(name) is None:
            disk_ver = self._schema_ver_from_meta()
            try:
                old = int(disk_ver) < int(SCHEMA_VERSION)
            except ValueError:
                old = disk_ver != SCHEMA_VERSION
            if old:
                raise schema_outdated(
                    "схема БД устарела: schema_ver %s, модуль ожидает %s; "
                    "таблица %r недоступна" % (disk_ver, SCHEMA_VERSION, name),
                    "переключитесь в rw-режим (db_set_mode readonly=false): "
                    "upgrade мигрирует схему и пересоздаст таблицу")

    def upgrade(self) -> dict:
        """Создаёт ВСЕ таблицы схемы (rw) для существующей БД, созданной до
        расширения (upgrade-path: read-only старт не может создавать dbi).

        Вызывается сервером при первом rw-старте (или явно через CLI). После
        создания — повторное открытие БД в readonly работает с полным набором.
        Также бэкфиллит covered_by из существующих coverage-записей.
        """
        created = []
        # N16 fast-path: если все таблицы уже открыты (Store.__init__
        # создаёт их на свежей БД) и backfill-маркер стоит — делать нечего,
        # не открываем write-txn вовсе (важно для read-heavy роя: upgrade
        # вызывается сервером на каждом rw-старте).
        if all(v is not None for v in self._dbi.values()) \
                and len(self._dbi) >= len(self.TABLES):
            try:
                with self.env.begin(readonly=True) as txn:
                    rc, val = txn.get(self.dbi(txn, "meta"),
                                      b"covered_by_backfilled")
                if val is not None:
                    return {"created": [], "covered_by_backfilled": 0,
                            "fast_path": True}
            except Exception:  # noqa: BLE001
                # Осознанно глотаем любую ошибку проверки маркера (в т.ч.
                # «meta не открывается») и идём обычным путём — он
                # идемпотентен; fast-path — чистая оптимизация, и диагностика
                # его промаха важнее, чем лишняя write-txn (ревью #164-r4).
                pass
        with self._begin_write() as txn:
            # ПРОХОД 1: открываем недостающие таблицы; для существующих с
            # НЕСОВМЕСТИМЫМИ флагами получаем old-handle и проверяем пустоту.
            # Непустая несовместимая таблица — явный отказ (C-2): drop без
            # архива уничтожил бы пользовательские данные. Всё в одной txn:
            # отказ откатывает и уже созданные таблицы (атомарность).
            mismatched = []  # (name, fl, old_handle)
            for name, fl in self.TABLES.items():
                dbi = self._dbi.get(name)
                if dbi is not None:
                    continue
                try:
                    self._dbi[name] = txn.open_dbi(name, fl | mdbx.CREATE)
                    created.append(name)
                except mdbx.LibmdbxError:
                    old = None
                    for cand_fl in (mdbx.DUPSORT,
                                    mdbx.DUPSORT | mdbx.INTEGERKEY,
                                    mdbx.DUPSORT | mdbx.DUPFIXED,
                                    mdbx.DB_DEFAULTS,
                                    mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP):
                        try:
                            old = txn.open_dbi(name, cand_fl)
                            break
                        except mdbx.LibmdbxError:
                            continue
                    if old is None:
                        raise
                    with txn.cursor(old) as cur:
                        rc, _, _ = cur.get(mdbx.CURSOR_FIRST)
                    if rc == mdbx.RC_SUCCESS:
                        mismatched.append(name)
                    else:
                        mismatched.append((name, fl, old))
            bad = [m for m in mismatched if isinstance(m, str)]
            if bad:
                raise invalid(
                    "schema-mismatch: таблицы %s существуют с другими флагами "
                    "и НЕ ПУСТЫ — автоматический drop уничтожил бы данные"
                    % ", ".join(bad),
                    "сделайте db_backup, затем пересоздайте БД и загрузите "
                    "данные заново (load_map/коллекторы)")
            # ПРОХОД 2: пересоздаём пустые несовместимые таблицы
            for name, fl, old in mismatched:
                txn.drop_dbi(old, del_=True)
                self._dbi[name] = txn.open_dbi(name, fl | mdbx.CREATE)
                created.append(name)
        # backfill covered_by из существующих coverage (однократно, маркер)
        marker = "covered_by_backfilled"
        n = 0
        with self._begin_write() as txn:
            meta = self.dbi(txn, "meta")
            rc, val = txn.get(meta, marker.encode())
            if val is None:
                cov_dbi = self.dbi(txn, "coverage")
                cb_dbi = self.dbi(txn, "covered_by")
                with txn.cursor(cov_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                    while rc == mdbx.RC_SUCCESS:
                        key = k.decode()
                        if key.startswith("coverage:"):
                            test_id = key[len("coverage:"):]
                            for sym in json.loads(v).get("symbols", []):
                                cb_key = ("covered_by:" + sym).encode()
                                rc2, val2 = txn.get(cb_dbi, cb_key)
                                cur2 = json.loads(val2.decode()) if val2 else []
                                if test_id not in cur2:
                                    cur2.append(test_id)
                                    txn.put(cb_dbi, cb_key,
                                            json.dumps(cur2).encode())
                                    n += 1
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                txn.put(meta, marker.encode(), str(n).encode())
        return {"created": created, "covered_by_backfilled": n}

    # --- helpers --------------------------------------------------------------
    def _check_size(self, key: bytes, value: bytes) -> None:
        klen = self.env.maxkeysize()
        if len(key) > klen:
            raise size_limit(
                "ключ %d байт > лимита движка %d байт" % (len(key), klen),
                "сократите ключ",
            )
        # прикладной лимит значения (движок допускает до ~2 GiB)
        if len(value) > self.max_value_bytes:
            raise size_limit(
                "значение %d байт > лимита %d байт" % (len(value), self.max_value_bytes),
                "сократите запись или разбейте на несколько записей",
            )

    def _begin_write(self):
        """Write-txn с ретраями по BUSY (класс busy-io)."""
        if self._readonly:
            raise readonly_mode(
                "хранилище открыто в read-only режиме",
                "вызовите db_set_mode(readonly=false) для переключения")
        self._check_deleted()
        self._raise_if_deleted()
        last_err = None
        for attempt in range(BUSY_RETRIES + 1):
            try:
                txn = self.env.begin(readonly=False)
                txn._store = self  # сбор commit-latency автоматически
                return txn
            except mdbx.LibmdbxError as e:
                if e.rc != mdbx.RC_BUSY:
                    raise
                last_err = e
                # D4/N12: наблюдаемость контеншена. СЕМАНТИКА (ревью #164-r4):
                # считается КАЖДАЯ BUSY-попытка begin, включая финальную
                # неудачную — при исчерпании ретраев busy_retries += RETRIES+1
                # и busy_failures += 1; поэтому busy_retries/busy_failures —
                # НЕ среднее число ретраев до успеха (это «всего BUSY-ударов»).
                self._busy_retries += 1
                if attempt < BUSY_RETRIES:
                    time.sleep(0.05 * (attempt + 1))
        self._busy_failures += 1
        raise busy_io(desc=str(last_err), action="повторите операцию позже")

    # --- records ---------------------------------------------------------------
    def exists(self, key: str) -> bool:
        with self.env.begin(readonly=True) as txn:
            rc, _ = txn.get(self.dbi(txn, "records"), key.encode())
            return rc == mdbx.RC_SUCCESS

    def touch(self, key: str) -> dict:
        """Фиксирует обращение к записи (LRU): увеличивает access_count."""
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            rc, val = txn.get(rec_dbi, key.encode())
            if val is None:
                raise invalid(
                    "запись %r не существует" % key,
                    "сначала сохраните её через safe_store",
                )
            body = json.loads(val.decode())
            self._touch_access(txn, body.get("_id", 0), score=None, count_delta=1)
        return {"touched": key}

    def _bump_access(self, key: str, force: bool = False) -> None:
        """Отложенная регистрация касания LRU (in-memory, без записи).

        Сама запись в access произойдёт одним батчем (_flush_touches).
        Rate-limit 60с на запись — per-process, чтобы гасить повторные чтения.
        """
        now = time.time()
        if not force and now - self._lru_last.get(key, 0.0) < LRU_RATE_LIMIT_SECONDS:
            return
        self._lru_last[key] = now
        self._touch_ids.append(key)

    def _flush_touches(self) -> None:
        """Одна write-txn на все накопленные LRU-касания (вместо N мелких).

        Rate-limit — только per-process (_lru_last в _bump_access): ts в
        access-записи трогать нельзя — он же источник recency для score,
        а safe_store пишет его свежим (D1-заметка: межпроцессная дедупликация
        касаний потребовала бы отдельного поля, не ts).
        """
        if not self._touch_ids:
            return
        pending, self._touch_ids = self._touch_ids, []
        try:
            with self._begin_write() as txn:
                ids_dbi = self.dbi(txn, "ids")
                for key in pending:
                    _, idb = txn.get(ids_dbi, key.encode())
                    if idb is None:
                        continue
                    self._touch_access(txn, _unpack_u64(idb),
                                       score=None, count_delta=1)
        except MemoryError:
            pass  # сбой LRU-метаданных не критичен для чтения

    def get_record(self, key: str):
        """Возвращает dict тела записи или None."""
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "records"), key.encode())
            return json.loads(val.decode()) if val is not None else None

    def _body(self, rtype: str, summary: str, importance: float, date: str,
              record_id: int, terms: list) -> bytes:
        body = {
            "type": rtype,
            "summary": summary,
            "importance": float(importance),
            "date": date,
            "_id": record_id,
            "_terms": terms,
        }
        return json.dumps(body, ensure_ascii=False, sort_keys=True).encode("utf-8")

    @staticmethod
    def _vocab_meta(val) -> dict:
        """Мета vocab-записи: {"refs": n, "auto": bool}.

        Обратная совместимость: старое значение b"{}" (или не-JSON) —
        ручная запись без ссылок (auto=False, защищена от gc-чистки).
        """
        if not val:
            return {"refs": 0, "auto": False}
        try:
            d = json.loads(val.decode())
        except ValueError:
            return {"refs": 0, "auto": False}
        return {"refs": int(d.get("refs", 0)), "auto": bool(d.get("auto", False))}

    def _vocab_bump_locked(self, txn, vkeys: list, delta: int,
                           auto: bool = False) -> None:
        """Изменяет refs vocab-записей (внутри write-txn).

        delta=+1 при создании записи знаний, -1 при purge. Пол не ниже 0.
        auto=True помечает запись как авто-зарегистрированную (кандидат на
        gc-чистку при refs==0); ручные vocab_add записи auto не получают.
        """
        voc_dbi = self.dbi(txn, "vocab")
        for vk in vkeys:
            k = vk if isinstance(vk, bytes) else vk.encode()
            rc, val = txn.get(voc_dbi, k)
            meta = self._vocab_meta(val)
            if val is None:
                meta = {"refs": 0, "auto": bool(auto)}
            meta["refs"] = max(0, meta["refs"] + delta)
            if auto:
                meta["auto"] = True
            txn.put(voc_dbi, k, json.dumps(meta).encode())

    def safe_store(self, raw_key: str, rtype: str, summary: str, importance: float,
                   date: str = "", auto_vocab: bool = True):
        """Атомарная операция сохранения с дедупликацией. Одна write-txn.

        ``auto_vocab`` — разрешить автоматическую регистрацию новой
        модуля/темы (в памяти и в таблице vocab) вместо ошибки invalid.
        По умолчанию включено: новые темы в потоке рефакторинга встречаются
        постоянно, а явный ``vocab_add`` остаётся для канонизации.
        """
        if rtype not in RECORD_TYPES:
            raise invalid(
                "тип %r не из допустимого набора" % rtype,
                "используйте один из: %s" % ", ".join(RECORD_TYPES),
            )
        key = self.norm.normalize(raw_key, register=auto_vocab)
        date = date or time.strftime("%Y-%m-%d", time.gmtime())
        try:
            importance = float(importance)
        except (TypeError, ValueError):
            raise invalid("Важность должна быть числом 0..1", "передайте число") from None
        importance = max(0.0, min(1.0, importance))

        summary = (summary or "").strip()
        if not summary:
            raise invalid("Суть записи пуста", "заполните поле summary")

        terms = idx.tokenize(summary)

        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            rc, old_val = txn.get(rec_dbi, key.encode())
            if rc == mdbx.RC_SUCCESS:
                return self._merge(txn, key, rtype, summary, importance, date,
                                   terms, old_val)
            # новая запись: SimHash-дедуп
            dup = self._find_conflict(txn, summary)
            if dup is not None:
                return {"result": "conflict", "key": dup}

            # авто-регистрация новых модуля/темы в vocab (persist), чтобы после
            # перезапуска словарь не потерял тему, появившуюся в потоке работы.
            # refs-счётчик: +1 за каждую запись, использующую модуль/тему —
            # gc() вычистит авто-записи, потерявшие все ссылки (C-7).
            _t, _m, _tm = key.split(":", 2)
            if auto_vocab:
                voc_dbi = self.dbi(txn, "vocab")
                mkey = b"module:" + _m.encode()
                if txn.get(voc_dbi, mkey)[1] is None:
                    txn.put(voc_dbi, mkey, b"{}")
                if txn.get(voc_dbi, key.encode())[1] is None:
                    txn.put(voc_dbi, key.encode(), b"{}")
            self._vocab_bump_locked(txn, ["module:" + _m, key], +1,
                                    auto=bool(auto_vocab))

            record_id = self._alloc_id_locked(txn)

            self._put_record(txn, key, self._body(rtype, summary, importance,
                                                  date, record_id, terms))
            self._index_add(txn, key, record_id, terms, summary)
            return {"result": "created", "key": key, "id": record_id}

    def _merge(self, txn, key, rtype, summary, importance, date, terms, old_val):
        old = json.loads(old_val.decode())
        old_terms = old.get("_terms", [])
        record_id = old.get("_id")
        if record_id is None:
            # старая запись без _id (например, ручной импорт/легаси) — выделяем
            # новый id из того же sequence, иначе коллизия в общем id-пространстве
            record_id = self._alloc_id_locked(txn)
        # история старой версии
        hist_key = "_history:%s:%d" % (key, _now())
        txn.put(self.dbi(txn, "history"), hist_key.encode(), old_val)
        self._put_record(txn, key, self._body(rtype, summary, importance,
                                              date, record_id, terms))
        self._index_update_terms(txn, key, record_id, old_terms, terms)
        self._index_add_terms(txn, record_id, terms)
        new_hash = idx.simhash(summary)
        txn.put(self.dbi(txn, "simhash"), _pack_u64(record_id), _pack_u64(new_hash))
        self._touch_access(txn, record_id, score=None, count_delta=0)
        return {"result": "merged", "key": key, "id": record_id}

    def _put_record(self, txn, key: str, body: bytes) -> None:
        k = key.encode()
        self._check_size(k, body)
        txn.put(self.dbi(txn, "records"), k, body, mdbx.PUT_UPSERT)

    def _index_add(self, txn, key, record_id, terms, summary) -> None:
        self._index_add_terms(txn, record_id, terms)
        txn.put(self.dbi(txn, "ids"), key.encode(), _pack_u64(record_id))
        txn.put(self.dbi(txn, "id2key"), _pack_u64(record_id), key.encode())
        h = idx.simhash(summary)
        txn.put(self.dbi(txn, "simhash"), _pack_u64(record_id), _pack_u64(h))
        self._touch_access(txn, record_id, score=None, count_delta=0)

    def _index_add_terms(self, txn, record_id, terms) -> None:
        inv_dbi = self.dbi(txn, "inverted")
        with txn.cursor(inv_dbi) as cur:
            idb = _pack_u64(record_id)
            for term in terms:
                cur.put_nodupe(term.encode(), idb)

    def _index_update_terms(self, txn, key, record_id, old_terms, new_terms) -> None:
        """Убирает из inverted термы, которых больше нет в записи."""
        removed = set(old_terms) - set(new_terms)
        if not removed:
            return
        inv_dbi = self.dbi(txn, "inverted")
        idb = _pack_u64(record_id)
        for term in removed:
            txn.delete(inv_dbi, term.encode(), idb)

    def _touch_access(self, txn, record_id, score=None, count_delta=1) -> None:
        acc_dbi = self.dbi(txn, "access")
        packed = _pack_u64(record_id)
        rc, old = txn.get(acc_dbi, packed)
        if old is not None and len(old) == ACCESS_LEN:
            old_score = struct_double(old[0:8])
            old_cnt = _unpack_u64(old[16:24])
        else:
            old_score = score if score is not None else 0.5
            old_cnt = 0
        new_score = score if score is not None else old_score
        new_ts = _now()
        new_cnt = old_cnt + max(count_delta, 0)
        txn.put(acc_dbi, packed, pack_access(new_score, new_ts, new_cnt))

    def _find_conflict(self, txn, summary: str):
        """SimHash Hamming <= 3 → ключ ближайшей записи (или None)."""
        h = idx.simhash(summary)
        sim_dbi = self.dbi(txn, "simhash")
        id2key_dbi = self.dbi(txn, "id2key")
        with txn.cursor(sim_dbi) as cur:
            rc, idk, val = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                other = _unpack_u64(val)
                if idx.hamming(h, other) <= 3:
                    _, k = txn.get(id2key_dbi, idk)
                    if k is not None:
                        return k.decode("utf-8", "replace")
                rc, idk, val = cur.get(mdbx.CURSOR_NEXT)
        return None

    # --- recall / search / lookup ---------------------------------------------
    def _score_of(self, txn, record_id: int, importance: float, incoming: int) -> float:
        acc_dbi = self._dbi["access"]
        rc, packed = txn.get(acc_dbi, _pack_u64(record_id))
        if packed and len(packed) == ACCESS_LEN:
            _, ts, cnt = unpack_access(packed)
            days = max(0.0, (_now() - ts) / 86400.0)
            recency = math.exp(-0.05 * days)
            access_factor = min(cnt, 10) / 10.0
        else:
            recency = 1.0
            access_factor = 0.0
        link_factor = min(incoming, 5) / 5.0
        return (0.35 * importance + 0.25 * recency + 0.20 * access_factor
                + 0.20 * link_factor)

    def recall(self, pattern: str, limit: int = 5, touch: bool = False):
        """Поиск по префиксу ключа с ранжированием.

        `touch=False` (D1/N9): чтение НЕ накачивает LRU-статистику —
        дашборды и массовые recall роя не искажают recency/access score
        и не порождают write-txn касаний. touch=True — прежнее поведение
        (осознанное «использование» записи).
        """
        limit = max(1, min(int(limit), 10))
        prefix = pattern[:-1] if pattern.endswith("*") else pattern
        out = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_SET_RANGE, prefix.encode())
                pbytes = prefix.encode()
                while rc == mdbx.RC_SUCCESS:
                    if not key.startswith(pbytes):
                        break
                    body = json.loads(val.decode())
                    record = self._decorate(txn, key.decode(), body)
                    out.append(record)
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        out.sort(key=lambda r: r.get("score", 0.0), reverse=True)
        top = out[:limit]
        if touch:
            for r in top:
                self._bump_access(r["key"])
            self._flush_touches()
        return top

    def keys(self, pattern: str = "", limit: int = None) -> list:
        """Все ключи записей по префиксу (без cap в 10, как у recall)."""
        prefix = pattern[:-1] if pattern.endswith("*") else pattern
        pbytes = prefix.encode()
        out = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, _ = cur.get(mdbx.CURSOR_SET_RANGE, pbytes)
                while rc == mdbx.RC_SUCCESS:
                    if not key.startswith(pbytes):
                        break
                    out.append(key.decode("utf-8", "replace"))
                    if limit is not None and len(out) >= limit:
                        break
                    rc, key, _ = cur.get(mdbx.CURSOR_NEXT)
        return out

    def _decorate(self, txn, key, body):
        incoming = self._incoming_count(txn, body.get("_id"))
        score = self._score_of(txn, body.get("_id", 0), body.get("importance", 0.0), incoming)
        return {
            "key": key,
            "type": body.get("type"),
            "summary": body.get("summary"),
            "importance": body.get("importance"),
            "date": body.get("date"),
            "score": round(score, 4),
            "links_in": incoming,
        }

    def _incoming_count(self, txn, record_id: int) -> int:
        """Число входящих рёбер узла.

        Источник — денормализованный счётчик `links_in` (обновляется при
        link/unlink/purge). Если записи-счётчика нет (старые/мигрированные
        узлы) — fallback на скан обратных рёбер в links.
        """
        li_dbi = self._opt_dbi(txn, "links_in")
        packed = None
        if li_dbi is not None:
            rc, packed = txn.get(li_dbi, _pack_u64(record_id))
        if packed and len(packed) == ID_LEN:
            return _unpack_u64(packed)
        links_dbi = self.dbi(txn, "links")
        prefix = _pack_u64(record_id) + b"\0" + b"back:"
        with txn.cursor(links_dbi) as cur:
            rc, key, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
            n = 0
            while rc == mdbx.RC_SUCCESS:
                if not key.startswith(prefix):
                    break
                n += 1
                rc, key, _ = cur.get(mdbx.CURSOR_NEXT)
            return n

    def _links_in_delta(self, txn, record_id: int, delta: int) -> None:
        """Денормализованное обновление счётчика входящих рёбер."""
        li_dbi = self.dbi(txn, "links_in")
        packed = _pack_u64(record_id)
        rc, old = txn.get(li_dbi, packed)
        n = _unpack_u64(old) if old and len(old) == ID_LEN else 0
        n = max(0, n + delta)
        txn.put(li_dbi, packed, _pack_u64(n))

    def lookup(self, term: str, limit: int = 50):
        """Точный поиск по инвертированному индексу."""
        words = idx.tokenize(term)
        if not words:
            return []
        id_sets = []
        with self.env.begin(readonly=True) as txn:
            inv_dbi = self.dbi(txn, "inverted")
            id2key_dbi = self.dbi(txn, "id2key")
            for w in words:
                s = set(self._posting(txn, inv_dbi, w))
                if not s:
                    return []
                id_sets.append(s)
            common = id_sets[0]
            for s in id_sets[1:]:
                common &= s
            keys = []
            for rec_id in sorted(common):
                if len(keys) >= limit:
                    break
                _, k = txn.get(id2key_dbi, _pack_u64(rec_id))
                if k is not None:
                    keys.append(k.decode("utf-8", "replace"))
        return keys

    def _posting(self, txn, inv_dbi, term: str) -> list:
        """Постинг-список терма (сортированный, из INTEGERDUP-набора)."""
        out = []
        with txn.cursor(inv_dbi) as cur:
            rc, _, _ = cur.get(mdbx.CURSOR_SET, term.encode())
            if rc != mdbx.RC_SUCCESS:
                return out
            rc, _, dval = cur.get(mdbx.CURSOR_FIRST_DUP)
            while rc == mdbx.RC_SUCCESS:
                out.append(_unpack_u64(dval))
                rc, _, dval = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def _structural_hits_locked(self, txn, query: str) -> list:
        """Записи структурного слоя по запросу (внутри активной txn).

        Срабатывает на явные префиксы идентификаторов (fn:/type:/macro:/
        group:/region:/uncovered:) и на совпадение последнего сегмента
        fq-имени символа с термом запроса (фасетный поиск «по коду»).
        """
        out = []
        q = query.strip().lower()
        prefixes = ("fn:", "type:", "macro:", "group:", "region:", "uncovered:")
        sym_dbi = self.dbi(txn, "symbols")
        gr_dbi = self.dbi(txn, "groups")
        seen = set()
        for p in prefixes:
            if not q.startswith(p):
                continue
            with txn.cursor(sym_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, q.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(q):
                        break
                    if key not in seen:
                        seen.add(key)
                        body = json.loads(v) if v else {}
                        out.append({"kind": "symbol", "key": key,
                                    "score": 1.0, "body": body})
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        if not out:
            words = idx.tokenize(q)
            with txn.cursor(sym_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    tail = key.rpartition(":")[2]
                    if any(len(w) > 2 and w in tail for w in words):
                        if key not in seen:
                            seen.add(key)
                            out.append({"kind": "symbol", "key": key,
                                        "score": 0.8,
                                        "body": json.loads(v) if v else {}})
                            if len(out) >= 8:
                                break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        if q.startswith("group:"):
            with txn.cursor(gr_dbi) as cur:
                rc, k, _ = cur.get(mdbx.CURSOR_SET_RANGE, q.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(q):
                        break
                    if key not in seen:
                        seen.add(key)
                        out.append({"kind": "group", "key": key,
                                    "score": 0.8, "body": {}})
                    rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
        return out[:8]

    def search(self, query: str, limit: int = 5, include_structural: bool = True,
               touch: bool = False):
        """Поиск по термам с query-aware ранжированием.

        `touch=False` (D1/N9): чтение не накачивает LRU (см. recall).

        Ранжирование BM25-подобное: для каждого терма запроса учитывается
        IDF (редкие термины весомее частых) в дополнение к композитному
        score записи (важность/recency/access/links). Это существенно лучше
        простого пересечения постинг-списков: возрастает релевантность
        узкоспециализированных терминов проекта (pnl, bigfoot, rkl, troika).

        ``include_structural`` — дополнительно включать в выдачу записи
        структурного слоя (symbols/groups), если запрос похож на идентификатор
        символа/фаcеты. Даёт единую точку входа «код + знания».
        Возвращает список, содержащий записи знаний (kind="record") и при
        совпадении — элементы структурного слоя (kind="symbol"/"group").
        """
        limit = max(1, min(int(limit), 20))
        rows = []
        with self.env.begin(readonly=True) as txn:
            if include_structural:
                rows.extend(self._structural_hits_locked(txn, query))
            words = idx.tokenize(query)
            if words:
                inv_dbi = self.dbi(txn, "inverted")
                rec_dbi = self.dbi(txn, "records")
                # число документов считаем в этой же txn (вложенный read-txn
                # в одном потоке запрещён: MDBX_BAD_RSLOT)
                n_docs = 0
                with txn.cursor(rec_dbi) as cur:
                    rc, _, _ = cur.get(mdbx.CURSOR_FIRST)
                    while rc == mdbx.RC_SUCCESS:
                        n_docs += 1
                        rc, _, _ = cur.get(mdbx.CURSOR_NEXT)
                postings = {}
                for w in words:
                    postings[w] = self._posting(txn, inv_dbi, w)
                id_sets = [s for s in postings.values() if s]
                if id_sets:
                    common = set(id_sets[0])
                    for s in id_sets[1:]:
                        common &= set(s)
                    for rec_id in common:
                        _, k = txn.get(self.dbi(txn, "id2key"), _pack_u64(rec_id))
                        if k is None:
                            continue
                        key = k.decode("utf-8", "replace")
                        rc, val = txn.get(rec_dbi, key.encode())
                        if val is None:
                            continue
                        body = json.loads(val.decode())
                        row = self._decorate(txn, key, body)
                        # запросная релевантность: сумма IDF по совпавшим термам
                        idf_sum = 0.0
                        for w, plist in postings.items():
                            if rec_id in plist:
                                df = len(plist)
                                idf_sum += math.log(1.0 + (n_docs - df + 0.5) / (df + 0.5))
                        row["query_score"] = round(row.get("score", 0.0) * (0.5 + idf_sum), 4)
                        row["bm25"] = round(idf_sum, 4)
                        row["kind"] = "record"
                        rows.append(row)
        # sort — ВНЕ read-txn: при touch=True флэш касаний пишет, а
        # параллельная read-транзакция в том же потоке дала бы
        # MDBX_TXN_OVERLAPPING
        rows.sort(key=lambda r: r.get("query_score", r.get("score", 0.0)), reverse=True)
        top = rows[:limit]
        if touch:
            for r in top:
                if r.get("kind") == "record":
                    self._bump_access(r["key"])
            self._flush_touches()
        return top

    # --- links -----------------------------------------------------------------
    def link(self, subject: str, predicate: str, object_: str):
        skey = self.norm.normalize(subject)
        predicate = predicate.strip().lower()
        if not predicate or ":" in predicate or "\0" in predicate:
            raise invalid("предикат недопустим", "используйте один из: caused-by, "
                          "fixed-by, related-to, supersedes, part-of, depends-on")
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            _, sv = txn.get(rec_dbi, skey.encode())
            if sv is None:
                raise invalid("субъект %r не существует" % skey,
                              "сначала сохраните запись через safe_store")
            sid = json.loads(sv.decode()).get("_id")
            # объект может быть либо записью знаний, либо символом карты
            # (fn:/type:/macro: из symbols — мост curated ↔ structural)
            okey = object_.strip()
            if okey.startswith(("fn:", "type:", "macro:")):
                _, okey2 = txn.get(self.dbi(txn, "sym_ids"), okey.encode())
                if okey2 is None or len(okey2) != 8:
                    # символа ещё нет в карте, но ключ существует в symbols?
                    rc3, _ = txn.get(self.dbi(txn, "symbols"), okey.encode())
                    if rc3 != mdbx.RC_SUCCESS:
                        raise invalid(
                            "символ %r не существует" % okey,
                            "сначала загрузите карту через load_map")
                    oid = self._sym_id_locked(txn, okey)
                else:
                    oid = _unpack_u64(okey2)
            else:
                okey = self.norm.normalize(okey)
                _, ov = txn.get(rec_dbi, okey.encode())
                if ov is None:
                    raise invalid("объект %r не существует" % okey,
                                  "сначала сохраните запись через safe_store")
                oid = json.loads(ov.decode()).get("_id")
            links_dbi = self.dbi(txn, "links")
            fwd = _pack_u64(sid) + b"\0" + predicate.encode()
            back = _pack_u64(oid) + b"\0" + b"back:" + predicate.encode()
            with txn.cursor(links_dbi) as cur:
                added_f = cur.put_nodupe(fwd, _pack_u64(oid))
                cur.put_nodupe(back, _pack_u64(sid))
            if added_f:
                # денормализованный счётчик входящих рёбер у объекта (и его
                # обратного ребра)
                self._links_in_delta(txn, oid, +1)
            return {"result": "linked" if added_f else "exists"}

    def unlink(self, subject: str, predicate: str, object_: str) -> dict:
        skey = self.norm.normalize(subject)
        okey = self.norm.normalize(object_)
        predicate = predicate.strip().lower()
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            _, sv = txn.get(rec_dbi, skey.encode())
            _, ov = txn.get(rec_dbi, okey.encode())
            if sv is None or ov is None:
                return {"result": "notfound"}
            sid = json.loads(sv.decode()).get("_id")
            oid = json.loads(ov.decode()).get("_id")
            links_dbi = self.dbi(txn, "links")
            fwd = _pack_u64(sid) + b"\0" + predicate.encode()
            back = _pack_u64(oid) + b"\0" + b"back:" + predicate.encode()
            r1 = txn.delete(links_dbi, fwd, _pack_u64(oid))
            txn.delete(links_dbi, back, _pack_u64(sid))
            if r1:
                self._links_in_delta(txn, oid, -1)
            return {"result": "unlinked" if r1 else "notfound"}

    def graph(self, key: str, depth: int = 1):
        depth = max(0, min(int(depth), 3))
        root = self.norm.normalize(key)
        nodes = {root}
        edges = []
        visited = set()
        frontier = [root]
        # ОДНА read-txn: проверка существования root и BFS из одного снимка
        # (B1) — раньше проверка была отдельной txn (TOCTOU: запись могли
        # удалить между проверкой и обходом).
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            rc, _ = txn.get(rec_dbi, root.encode())
            if rc != mdbx.RC_SUCCESS:
                raise invalid(
                    "запись %r не существует" % root,
                    "сначала сохраните её через safe_store",
                )
            links_dbi = self.dbi(txn, "links")
            for _ in range(depth + 1):
                nxt = []
                for node in frontier:
                    if node in visited:
                        continue
                    visited.add(node)
                    _, v = txn.get(rec_dbi, node.encode())
                    if v is None:
                        continue
                    rid = json.loads(v.decode()).get("_id")
                    prefix = _pack_u64(rid) + b"\0"
                    with txn.cursor(links_dbi) as cur:
                        rc, lk, lv = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
                        while rc == mdbx.RC_SUCCESS:
                            if not lk.startswith(prefix):
                                break
                            pred = lk[ID_LEN + 1:].decode()
                            if pred.startswith("back:"):
                                # обратная связь: ребро object -> subject
                                obj_id = _unpack_u64(lv)
                                tgt = self._graph_resolve(txn, obj_id)
                                if tgt:
                                    edges.append({"subject": tgt,
                                                  "predicate": pred[5:],
                                                  "object": node})
                                    nxt.append(tgt)
                            else:
                                obj_id = _unpack_u64(lv)
                                tgt = self._graph_resolve(txn, obj_id)
                                if tgt:
                                    edges.append({"subject": node,
                                                  "predicate": pred,
                                                  "object": tgt})
                                    nxt.append(tgt)
                            rc, lk, lv = cur.get(mdbx.CURSOR_NEXT)
                frontier = [n for n in nxt if n not in nodes]
                for n in frontier:
                    nodes.add(n)
        seen = set()
        unique = []
        for e in edges:
            sig = (e["subject"], e["predicate"], e["object"])
            if sig not in seen:
                seen.add(sig)
                unique.append(e)
        return {"nodes": sorted(nodes), "edges": unique}

    def _graph_resolve(self, txn, oid: int) -> str:
        """Резолвит числовой id объекта в ключ: сначала запись знаний
        (id2key), затем символ карты (sym_id2key) — мост curated↔structural."""
        _, kobj = txn.get(self.dbi(txn, "id2key"), _pack_u64(oid))
        if kobj is not None:
            return kobj.decode()
        _, kobj = txn.get(self.dbi(txn, "sym_id2key"), _pack_u64(oid))
        if kobj is not None:
            return kobj.decode()
        return ""

    # --- vocab ------------------------------------------------------------------
    def vocab_add(self, module: str, topic: str = ""):
        module = module.strip().lower()
        with self._begin_write() as txn:
            voc_dbi = self.dbi(txn, "vocab")
            if not topic:
                canon = self.norm.add_module(module)
                key = b"module:" + canon.encode()
                rc, _ = txn.get(voc_dbi, key)
                if rc != mdbx.RC_SUCCESS:
                    txn.put(voc_dbi, key, b"{}")
                    return {"result": "added", "key": "module:%s" % canon}
                return {"result": "exists", "key": "module:%s" % canon}
            canon_m = self.norm.add_module(module)
            # модуль тоже персистится (иначе после перезапуска словарь потерян)
            mkey = b"module:" + canon_m.encode()
            rc, _ = txn.get(voc_dbi, mkey)
            if rc != mdbx.RC_SUCCESS:
                txn.put(voc_dbi, mkey, b"{}")
            canon_t = self.norm.add_topic(topic)
            key = ("%s:%s" % (canon_m, canon_t)).encode()
            rc, _ = txn.get(voc_dbi, key)
            if rc != mdbx.RC_SUCCESS:
                txn.put(voc_dbi, key, b"{}")
                return {"result": "added", "key": "%s:%s" % (canon_m, canon_t)}
            return {"result": "exists", "key": "%s:%s" % (canon_m, canon_t)}

    def vocab_find(self, query: str, n: int = 5):
        modules = self.norm.fuzzy_modules(query, n=n)
        topics = self.norm.fuzzy_topics(query, n=n)
        return {"modules": modules, "topics": topics}

    def vocab_list(self):
        return {"modules": sorted(self.norm.modules), "topics": sorted(self.norm.topics)}

    # --- context dump/restore -----------------------------------------------------
    def dump_context(self, task: str = "", milestone: str = "",
                     keys: list = None, hypotheses: list = None) -> dict:
        ctx = {
            "type": "proc",
            "task": task,
            "milestone": milestone,
            "keys": keys or [],
            "hypotheses": hypotheses or [],
            "events": self.recall("event:", limit=5),
        }
        ts = int(time.time())
        key = "proc:memory:context-snapshot"
        with self._begin_write() as txn:
            record_id = self._alloc_id_locked(txn)
            ctx["_id"] = record_id
            ctx["_terms"] = []
            body = json.dumps(ctx, ensure_ascii=False, sort_keys=True).encode()
            k = key.encode()
            self._check_size(k, body)
            txn.put(self.dbi(txn, "records"), k, body)
            txn.put(self.dbi(txn, "ids"), k, _pack_u64(record_id))
            txn.put(self.dbi(txn, "id2key"), _pack_u64(record_id), k)
        return {"context_id": str(ts), "key": key}

    def restore_context(self, context_id: str) -> dict:
        return self.recall("proc:memory:context-snapshot", limit=1)

    # --- gc / stats / purge -------------------------------------------------------
    def _all_records(self):
        """Все записи records в память (корпус мал; избегаем вложенных txn)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    out.append((key, val))
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        return out

    def _gc_vocab_locked(self, txn) -> int:
        """Удаляет авто-зарегистрированные vocab-записи с refs==0 (C-7).

        Ручные (auto=False) и записи со ссылками защищены. Модуль удаляется,
        только если auto и refs==0; тема — если auto и refs==0. Возвращает
        число удалённых записей.
        """
        voc_dbi = self._opt_dbi(txn, "vocab")
        if voc_dbi is None:
            return 0
        to_delete = []
        with txn.cursor(voc_dbi) as cur:
            rc, k, v = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                meta = self._vocab_meta(v)
                if meta.get("auto") and meta.get("refs", 0) <= 0:
                    to_delete.append(bytes(k))
                rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        for k in to_delete:
            txn.delete(voc_dbi, k)
        return len(to_delete)

    def gc(self, dry_run: bool = True, archive: bool = False, threshold: float = GC_WARM):
        hot, warm, cold = [], [], []
        ids_to_cold = []
        # ОДНА read-txn классификации (B1): records + счётчики входящих из
        # одного снимка — раньше на каждую запись открывалась своя txn (N+1).
        # Тиринг фиксируется консистентно; запись (archive/delete) ниже — в
        # своей write-txn, как и прежде.
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    body = json.loads(val.decode())
                    if body.get("type") == "proc" and \
                            "context-snapshot" in key.decode():
                        hot.append((key, None))
                        rc, key, val = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    rec_id = body.get("_id", 0)
                    incoming = self._incoming_count(txn, rec_id)
                    score = self._score_of(txn, rec_id,
                                           body.get("importance", 0.0),
                                           incoming)
                    item = (key, score)
                    if score > GC_HOT:
                        hot.append(item)
                    elif score > threshold:
                        warm.append(item)
                    else:
                        cold.append(item)
                        ids_to_cold.append((key, rec_id, body))
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        stats = {
            "hot": len(hot), "warm": len(warm), "cold": len(cold),
            "dry_run": dry_run, "archive": archive,
            "cold_keys": [k.decode() if isinstance(k, bytes) else k for k, _ in cold],
        }
        vocab_removed = 0
        if not dry_run:
            # ЕДИНАЯ write-txn: архивация (если есть) + чистка vocab (C-7).
            moved = 0
            with self._begin_write() as txn:
                if archive and cold:
                    arch_dbi = self.dbi(txn, "archive")
                    for key, rec_id, body in ids_to_cold:
                        txn.put(arch_dbi, key,
                                json.dumps(body, ensure_ascii=False).encode())
                        self._delete_record(txn, key.decode(), body,
                                            vocab_refs=False)
                        moved += 1
                vocab_removed = self._gc_vocab_locked(txn)
            if archive and cold:
                stats["archived"] = moved
        stats["vocab_removed"] = vocab_removed
        self.last_gc = _now()
        return stats

    def purge(self, keys: list):
        """Явное удаление записей (и их индексов)."""
        removed, missing = [], []
        for raw in keys:
            key = self.norm.normalize(raw)
            with self._begin_write() as txn:
                rec_dbi = self.dbi(txn, "records")
                rc, val = txn.get(rec_dbi, key.encode())
                if val is None:
                    missing.append(key)
                    continue
                body = json.loads(val.decode())
                self._delete_record(txn, key, body)
                removed.append(key)
        return {"removed": removed, "missing": missing}

    def _delete_record(self, txn, key: str, body: dict,
                       vocab_refs: bool = True) -> None:
        rec_id = body.get("_id")
        terms = body.get("_terms", [])
        rec_dbi = self.dbi(txn, "records")
        ids_dbi = self.dbi(txn, "ids")
        id2key_dbi = self.dbi(txn, "id2key")
        inv_dbi = self.dbi(txn, "inverted")
        sim_dbi = self.dbi(txn, "simhash")
        acc_dbi = self.dbi(txn, "access")
        links_dbi = self.dbi(txn, "links")
        txn.delete(rec_dbi, key.encode())
        txn.delete(ids_dbi, key.encode())
        idb = _pack_u64(rec_id)
        txn.delete(id2key_dbi, idb)
        for term in terms:
            txn.delete(inv_dbi, term.encode(), idb)
        txn.delete(sim_dbi, idb)
        txn.delete(acc_dbi, idb)
        self._delete_links_of(txn, links_dbi, idb, rec_id)
        # денормализованный счётчик входящих этой записи: чистим и его
        # (F1-review #2) — id монотонны и не переиспользуются, сирота
        # безвреден, но оставляет мусор в links_in. Таблица опциональна
        # (старая БД) — только если открыта.
        li_dbi = self._opt_dbi(txn, "links_in")
        if li_dbi is not None:
            txn.delete(li_dbi, idb)
        # vocab refs: -1 за удалённую запись (модуль + тема).
        # При архивации (gc archive=True) НЕ декрементируем (A5-γ):
        # запись жива в archive, тема может вернуться.
        if vocab_refs:
            try:
                _t, _m, _tm = key.split(":", 2)
                self._vocab_bump_locked(txn, ["module:" + _m, key], -1)
            except ValueError:
                pass  # неканонический ключ — vocab не трогаем

    def _delete_links_of(self, txn, links_dbi, idb, rec_id) -> None:
        # Все рёбра записи адресуются ДВУМЯ точными префиксами (A4):
        #   pack(rec_id)+b"\0"            — исходящие fwd (субъект = rec_id),
        #   pack(rec_id)+b"\0back:"       — входящие обратные рёбра
        # (пара к fwd-ребру pack(sid)+b"\0"+pred со значением pack(rec_id)).
        # Полный скан links не нужен и при росте таблицы не держит write-txn.
        # Сначала собираем входящие fwd-рёбра (по back:-ключам), чтобы
        # удалить и их fwd-половины: иначе остались бы висячие fwd-рёбра.
        back_pre = idb + b"\0back:"
        incoming = []  # (fwd_key, sid) чужих fwd-рёбер на эту запись
        with txn.cursor(links_dbi) as cur:
            rc, lk, lv = cur.get(mdbx.CURSOR_SET_RANGE, back_pre)
            while rc == mdbx.RC_SUCCESS:
                if not lk.startswith(back_pre):
                    break
                sid = _unpack_u64(lv)
                pred = lk[ID_LEN + 1 + len(b"back:"):]
                incoming.append((_pack_u64(sid) + b"\0" + pred, sid))
                rc, lk, lv = cur.get(mdbx.CURSOR_NEXT)
        # удаляем ВСЕ рёбра записи одним префиксным проходом (fwd и back:
        # разделяют префикс pack(rec_id)+b"\0")
        prefix = idb + b"\0"
        with txn.cursor(links_dbi) as cur:
            rc, lk, lv = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
            while rc == mdbx.RC_SUCCESS:
                if not lk.startswith(prefix):
                    break
                pred = lk[ID_LEN + 1:]
                # счётчик входящих крутим только у fwd-рёбер (объект = lv);
                # у back:-рёбер lv — sid чужого fwd-ребра, его счётчик
                # учитывается при удалении fwd-половины ниже.
                if not pred.startswith(b"back:"):
                    self._links_in_delta(txn, _unpack_u64(lv), -1)
                cur.delete()
                rc, lk, lv = cur.get(mdbx.CURSOR_NEXT)
        # fwd-половины входящих рёбер (счётчик этой записи — в объекте lv,
        # т.е. она сама: декремент не нужен — запись удаляется целиком).
        for fwd_key, sid in incoming:
            txn.delete(links_dbi, fwd_key, idb)

    # --- refactoring-map (структурный слой) ---------------------------------
    def _map_symbol_locked(self, txn, pre: str, key: str) -> dict:
        """map_symbol внутри УЖЕ открытой read-txn (без своей txn)."""
        rc, val = txn.get(self.dbi(txn, "symbols"), (pre + key).encode())
        if val is None:
            return {}
        return json.loads(val.decode())

    def map_symbol(self, key: str, layer: str = "") -> dict:
        """Читает символ из structural-слоя (symbols).

        `layer` — слой ветки; базовый слой (""/ver) без префикса.
        Пустой layer — использует активную ветку (branch_current).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self.env.begin(readonly=True) as txn:
            return self._map_symbol_locked(txn, pre, key)

    def sym_id(self, key: str) -> int:
        """Стабильный числовой id символа.

        Числовой id выделяется из ОБЩЕГО счётчика `meta/next_id` (того же,
        что для записей знаний): links/coverage оперируют единым числовым
        пространством, пересечение диапазонов символов и записей было бы
        неоднозначностью. Отображения sym_ids/sym_id2key НЕ чистятся при
        replace — числовые ссылки (links) остаются стабильными между
        регенерациями. При `branch_purge` пары id↔key слоя УДАЛЯЮТСЯ
        (иначе сироты); links curated-записей на символы purged-слоя
        становятся висячими до `map_refresh_stale`.
        """
        with self._begin_write() as txn:
            sid_dbi = self.dbi(txn, "sym_ids")
            rc, val = txn.get(sid_dbi, key.encode())
            if val is not None and len(val) == 8:
                return _unpack_u64(val)
            nid = self._alloc_id_locked(txn)
            txn.put(sid_dbi, key.encode(), _pack_u64(nid))
            txn.put(self.dbi(txn, "sym_id2key"), _pack_u64(nid), key.encode())
            return nid

    def sym_key(self, sid: int) -> str:
        """Обратное отображение id → ключ символа."""
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "sym_id2key"), _pack_u64(sid))
        return val.decode() if val else ""

    def map_alias(self, old_key: str, new_key: str) -> dict:
        """Фиксирует переименование символа карты (жизненный цикл).

        Старый ключ помечается алиасом нового. При `refresh_stale` связь
        на исчезнувший символ с алиасом перенаправляется, а не удаляется
        безвозвратно — курируемые записи переживают рефакторинг.
        Возвращает {"result": "aliased"} или {"result": "exists"}.
        """
        old_key, new_key = old_key.strip(), new_key.strip()
        if not old_key or not new_key or old_key == new_key:
            raise invalid("алиас требует два разных ключа символов",
                          "old_key и new_key должны быть непустыми и разными")
        with self._begin_write() as txn:
            al_dbi = self.dbi(txn, "sym_aliases")
            rc, _ = txn.get(al_dbi, old_key.encode())
            if rc == mdbx.RC_SUCCESS:
                return {"result": "exists"}
            txn.put(al_dbi, old_key.encode(), new_key.encode())
        return {"result": "aliased"}

    def map_alias_target(self, old_key: str) -> str:
        """Цель алиаса (с учётом цепочек) или пустая строка."""
        with self.env.begin(readonly=True) as txn:
            return self._alias_target_locked(txn,
                                             self.dbi(txn, "sym_aliases"),
                                             old_key.strip())

    def _map_symbols_locked(self, txn, pre: str, prefix: str = "",
                            limit: int = None) -> list:
        """map_symbols внутри УЖЕ открытой read-txn (без своей txn)."""
        sym_dbi = self.dbi(txn, "symbols")
        mod_pat = None
        if prefix.startswith("fn:") and ":" not in prefix[3:].lstrip(":"):
            mod_pat = prefix[3:].lstrip(":")
        out = []
        with txn.cursor(sym_dbi) as cur:
            rc, k, v = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                key = k.decode()
                # базовый слой не должен видеть чужие feature-слои
                if not pre and _LAYER_KEY_RE.match(key):
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                    continue
                if pre and not key.startswith(pre):
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                    continue
                bare = key[len(pre):] if pre else key
                if bare.startswith(prefix):
                    out.append(bare)
                    if limit and len(out) >= limit:
                        break
                elif mod_pat:
                    # module-fallback ТОЛЬКО для fn:-пространства: не
                    # путать type:/macro: карты с функциями
                    if not bare.startswith("fn:"):
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    try:
                        body = json.loads(v)
                    except ValueError:
                        body = {}
                    # merged-карты хранят module внутри implementations
                    mods = [m.get("module")
                            for m in body.get("implementations") or []]
                    mods.append(body.get("module", ""))
                    if any(m and m.startswith(mod_pat) for m in mods):
                        out.append(bare)
                        if limit and len(out) >= limit:
                            break
                rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def map_symbols(self, prefix: str = "", limit: int = None,
                    layer: str = "") -> list:
        """Все ключи symbols по префиксу (детерминированная сортировка).

        `layer` — слой ветки; базовый слой без учёта префикса; feature-слой
        ищет только в своём префиксе (и возвращает ключи без префикса слоя).

        Исключение Кайдзен: канонические ключи функций — `fn:{qname}`
        (module вятел), поэтому префикс вида `fn:gc`/`fn:gc-` ДОПОЛНИтельно
        фильтрует по `module`-полю тела (аддитивно: поиск по модулю работает
        и при несовпадении префикса ключа с названием модуля).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self.env.begin(readonly=True) as txn:
            return self._map_symbols_locked(txn, pre, prefix, limit)

    def map_put_symbol(self, key: str, body: dict, layer: str = "") -> None:
        """Аддитивная запись символа (перегенерируемый слой).

        Присваивает стабильный числовой id при первом появлении.
        Пустой layer — активная ветка.
        `cpp_status` в body валидируется против CPP_STATUSES (N6): статус
        портирования C→C++ читает map_port_status — мусорное значение
        молча ушло бы в "none", поэтому лучше отклонить на входе.
        """
        key = (key or "").strip()
        if not key:
            raise invalid("map_put_symbol: пустой ключ символа",
                          "укажите fq-ключ fn:/type:/macro:")
        st = (body or {}).get("cpp_status")
        if st is not None and st not in CPP_STATUSES:
            raise invalid(
                "map_put_symbol: cpp_status %r не из допустимого набора" % st,
                "допустимые: %s" % ", ".join(CPP_STATUSES))
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        skey = (pre + key)
        with self._begin_write() as txn:
            txn.put(self.dbi(txn, "symbols"), skey.encode(),
                    json.dumps(body, ensure_ascii=False).encode())
            self._sym_id_locked(txn, skey)

    def map_edges_of(self, caller: str, layer: str = "") -> list:
        """callee-дубликаты из call_edges для caller (DUPSORT).

        `layer` — слой ветки: feature-слой адресуется через `layer/`-префикс;
        базовый слой (""/ver) — без префикса. Пустой — активная ветка.
        Возвращает layer-local ключи (префикс слоя снят), как map_symbols.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            ce_dbi = self.dbi(txn, "call_edges")
            with txn.cursor(ce_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY,
                                     (pre + caller).encode())
                while rc == mdbx.RC_SUCCESS:
                    try:
                        body = json.loads(val.decode())
                        cal = body.get("callee", "")
                        if pre and cal.startswith(pre):
                            body["callee"] = cal[len(pre):]
                        out.append(json.dumps(body, ensure_ascii=False))
                    except ValueError:
                        rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
                        continue
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_callers_of(self, callee: str, layer: str = "") -> list:
        """Обратные рёбра: кто вызывает callee (DUPSORT call_edges_rev).

        Возвращает layer-local ключи (префикс слоя снят).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            ce_dbi = self.dbi(txn, "call_edges_rev")
            with txn.cursor(ce_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY,
                                     (pre + callee).encode())
                while rc == mdbx.RC_SUCCESS:
                    caller = val.decode()
                    if pre and caller.startswith(pre):
                        caller = caller[len(pre):]
                    out.append(caller)
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_put_edge(self, caller: str, callee: str, kind: str = "syntax",
                     layer: str = "", configs: list = None) -> None:
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            ce_dbi = self.dbi(txn, "call_edges")
            body = {"kind": kind, "resolved": False,
                    "ambiguous": False, "callee": pre + callee}
            if configs:
                body["configs"] = sorted(set(configs))
            val = json.dumps(body, ensure_ascii=False).encode()
            with txn.cursor(ce_dbi) as cur:
                cur.put_nodupe((pre + caller).encode(), val)
            rev_dbi = self.dbi(txn, "call_edges_rev")
            with txn.cursor(rev_dbi) as cur:
                cur.put_nodupe((pre + callee).encode(),
                               (pre + caller).encode())

    def map_groups_of(self, group_key: str, layer: str = "") -> list:
        """Члены группы (DUPSORT-набор), layer-local ключи."""
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY,
                                     (pre + group_key).encode())
                while rc == mdbx.RC_SUCCESS:
                    m = val.decode()
                    if pre and m.startswith(pre):
                        m = m[len(pre):]
                    out.append(m)
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_group_members(self, prefix: str = "", layer: str = "") -> list:
        """Все группы с их членами по префиксу ключа группы (layer-local)."""
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                rc, k, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    if (pre and not key.startswith(pre)) or \
                       (not pre and _LAYER_KEY_RE.match(key)):
                        rc, k, val = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    gkey = key[len(pre):] if pre else key
                    m = val.decode()
                    if pre and m.startswith(pre):
                        m = m[len(pre):]
                    if not prefix or gkey.startswith(prefix):
                        out.append((gkey, m))
                    rc, k, val = cur.get(mdbx.CURSOR_NEXT)
        return out

    def map_put_group(self, group_key: str, member: str, layer: str = "") -> None:
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                cur.put_nodupe((pre + group_key).encode(),
                               (pre + member).encode())

    def map_load_batch(self, symbols: dict, edges: list, groups: dict,
                       chunk: int = 4096, replace: bool = False,
                       layer: str = "") -> dict:
        """Батчевая загрузка структурного слоя.

        Принимает готовые данные (из автогенератора) и пишет в
        symbols/call_edges/call_edges_rev/groups.
        `layer` — слой ветки (см. docs/BRANCHES.md): базовый слой (""/"0.15")
        пишет без префикса, feature-слои — с префиксом `{layer}/`.
        Пустой `layer` — использует активную ветку (branch_current), чтобы
        загрузка не расходилась с чтениями после branch_activate.
        При replace=True очищается только ТЕКУЩИЙ слой (feature-префиксы
        чужих веток защищены _LAYER_KEY_RE).
        `chunk` — размер порции элементов на одну write-txn (A2): коммит
        каждые `chunk` записей освобождает writer-лок между порциями, чтобы
        загрузка карты не вытесняла писателей роя в busy_io. Поколение
        (canary.y) инкрементируется один раз в финальной txn — читатели
        видят прежнее поколение вплоть до завершения. Загрузка идемпотентна:
        перезапуск после обрыва эквивалентен (очистка в первом чанке +
        upsert/put_nodupe).

        ОКНО ЧАСТИЧНОЙ ЗАГРУЗКИ (осознанный trade-off A2): между чанками
        replace-загрузки читатели могут видеть частично обновлённое
        содержимое, тогда как canary.y ещё сообщает ПРЕЖНЕЕ поколение (оно
        инкрементируется только в финальной txn). Поэтому `generation` — НЕ
        токен «всё-или-ничего» для содержимого во время загрузки: потребители
        кэша, ключующиеся только на generation, промежуточное состояние не
        заметят; `map_snapshot.content_hash` меняется и корректен. Обрыв
        между чанками оставляет слой очищенным-но-недогруженным до перезапуска
        (идемпотентный перезапуск покрывает это). Монолитная загрузка
        (chunk >= числа элементов) сохраняет прежнюю атомарность контент+gen.
        Возвращает счётчики для сверки.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        put_sym, put_edge, put_grp = 0, 0, 0
        sym_dbi = self._dbi["symbols"]
        ce_dbi = self._dbi["call_edges"]
        rev_dbi = self._dbi["call_edges_rev"]
        gr_dbi = self._dbi["groups"]
        sid_dbi = self._dbi["sym_ids"]
        id2key_dbi = self._dbi["sym_id2key"]
        # Рабочие элементы: ("sym", key, body) | ("edge", e) | ("grp", g, m).
        # Порядок детерминирован (символы отсортированы) — идемпотентность
        # перезапуска прерванной загрузки: put upsert/put_nodupe + очистка
        # слоя в первом чанке делают повтор эквивалентным.
        work = [("sym", k, b) for k, b in sorted(symbols.items())]
        work += [("edge", e, None) for e in edges]
        work += [("grp", gk, m) for gk, members in groups.items()
                 for m in members]
        # чанкинг по write-txn (A2): одна монолитная txn на ~25k put держит
        # writer-лок секунды и вытесняет писателей роя в busy_io; коммитим
        # каждые `chunk` элементов, освобождая лок между чанками.
        chunk = max(1, int(chunk or 0))
        chunks = [work[i:i + chunk] for i in range(0, len(work), chunk)] \
            or [[]]
        first = True
        for part in chunks:
            with self._begin_write() as txn:
                if replace and first:
                    # Очищаем только записи ТЕКУЩЕГО слоя. Важно: базовый слой
                    # (pre=="") не должен сносить feature-слои параллельных
                    # веток ("0.16.feat.N/...") — их защищает _LAYER_KEY_RE.
                    # Идемпотентно: повторная загрузка после обрыва между
                    # чанками начинается с той же очистки.
                    for dbi in (sym_dbi, ce_dbi, rev_dbi, gr_dbi):
                        with txn.cursor(dbi) as cur:
                            rc, k, _ = cur.get(mdbx.CURSOR_FIRST)
                            while rc == mdbx.RC_SUCCESS:
                                key = k.decode()
                                if (not pre and not _LAYER_KEY_RE.match(key)) \
                                   or (pre and key.startswith(pre)):
                                    cur.delete()
                                rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
                first = False
                for kind, k1, k2 in part:
                    if kind == "sym":
                        skey = (pre + k1).encode()
                        txn.put(sym_dbi, skey,
                                json.dumps(k2, ensure_ascii=False).encode())
                        put_sym += 1
                        # стабильный числовой id (НЕ чистится при replace)
                        rc, val = txn.get(sid_dbi, skey)
                        if val is None or len(val) != 8:
                            nid = self._alloc_id_locked(txn)
                            txn.put(sid_dbi, skey, _pack_u64(nid))
                            txn.put(id2key_dbi, _pack_u64(nid), skey)
                    elif kind == "edge":
                        # JSON-значение: kind, resolved, ambiguous, callee...
                        body = {
                            "kind": k1.get("kind", "syntax"),
                            "resolved": k1.get("resolved", False),
                            "ambiguous": k1.get("ambiguous", False),
                            "callee": pre + k1["callee"],
                        }
                        if k1.get("configs"):
                            body["configs"] = sorted(set(k1["configs"]))
                        val = json.dumps(body, ensure_ascii=False).encode()
                        with txn.cursor(ce_dbi) as cur:
                            cur.put_nodupe((pre + k1["caller"]).encode(), val)
                        put_edge += 1
                        # обратное ребро: callee → caller
                        with txn.cursor(rev_dbi) as cur:
                            cur.put_nodupe((pre + k1["callee"]).encode(),
                                           (pre + k1["caller"]).encode())
                    else:  # grp
                        with txn.cursor(gr_dbi) as cur:
                            cur.put_nodupe((pre + k1).encode(),
                                           (pre + k2).encode())
                        put_grp += 1
        if replace and not pre:
            # поколение карты — ОДИН раз на целую загрузку, в финальной txn:
            # промежуточные чанки не двигают generation (читатели видят
            # прежнее поколение до завершения, атомарность поколения сохранена)
            with self._begin_write() as txn:
                canary = txn.canary_get()
                txn.canary_put(x=canary.get("x", 0),
                               y=int(canary.get("y", 0)) + 1,
                               z=canary.get("z", 0))
        return {"symbols": put_sym, "edges": put_edge, "groups": put_grp}

    # --- canary (поколение карты / метки схемы) -------------------------
    def map_canary_get(self) -> dict:
        """Читает MDBX_canary env: x=magic схемы, y=поколение карты,
        z=резерв, v=номер транзакции последнего изменения."""
        with self.env.begin(readonly=True) as txn:
            return txn.canary_get()

    def map_canary_put(self, x: int = 0, y: int = 0, z: int = 0) -> None:
        with self._begin_write() as txn:
            txn.canary_put(x=x, y=y, z=z)

    def map_refresh_stale(self) -> dict:
        """Чистит висячие ссылки моста: объекты-символы в links, чьих ключей
        больше нет в symbols (после регенерации карты).

        Ссылка на исчезнувший символ с алиасом (`map_alias`) перенаправляется
        на цель алиаса; без алиаса — удаляется. Возвращает счётчики.
        """
        removed, redirected = 0, 0
        with self._begin_write() as txn:
            sym_id2key_dbi = self.dbi(txn, "sym_id2key")
            syms_dbi = self.dbi(txn, "symbols")
            al_dbi = self.dbi(txn, "sym_aliases")
            links_dbi = self.dbi(txn, "links")

            # символьные id, чей ключ есть в symbols (живые)
            alive = set()
            with txn.cursor(sym_id2key_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    sid = _unpack_u64(k)
                    key = v.decode() if v else ""
                    rc2, _ = txn.get(syms_dbi, key.encode())
                    if rc2 == mdbx.RC_SUCCESS:
                        alive.add(sid)
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)

            # собираем прямые рёбра (sid → oid), где oid — мёртвый символ
            fwd = []  # (fwd_key, sid, predicate, oid)
            with txn.cursor(links_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    oid = _unpack_u64(v)
                    _, key = txn.get(sym_id2key_dbi, _pack_u64(oid))
                    if key is not None and oid not in alive:
                        sid = _unpack_u64(k[:8])
                        predicate = k[9:].decode()
                        fwd.append((k, sid, predicate, oid))
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)

            for fwd_key, sid, predicate, oid in fwd:
                _, key = txn.get(sym_id2key_dbi, _pack_u64(oid))
                key = key.decode() if key else ""
                target = self._alias_target_locked(txn, al_dbi, key)
                back_key = _pack_u64(oid) + b"\0" + b"back:" + predicate.encode()
                if target:
                    tid = self._sym_id_locked(txn, target)
                    if tid is not None and tid != oid:
                        # перецепить прямую ссылку и обратное ребро
                        txn.delete(links_dbi, fwd_key)
                        txn.delete(links_dbi, back_key)
                        nk = _pack_u64(sid) + b"\0" + predicate.encode()
                        txn.put(links_dbi, nk, _pack_u64(tid))
                        nbk = _pack_u64(tid) + b"\0" + b"back:" + predicate.encode()
                        txn.put(links_dbi, nbk, _pack_u64(sid))
                        redirected += 1
                        continue
                txn.delete(links_dbi, fwd_key)
                txn.delete(links_dbi, back_key)
                removed += 1
        return {"removed_links": removed, "redirected_links": redirected}

    # --- жизненный цикл: stale-пометка + поколения карты ----------------------
    def map_mark_stale(self, live_keys: list, layer: str = "") -> dict:
        """Помечает символы, отсутствующие в live_keys, как stale.

        Работает ТОЛЬКО по активному/явному слою: не задевает чужие ветки.
        В отличие от `map_refresh_stale` (удаляющего висячие ссылки), этот
        метод НЕ удаляет записи symbols: проставляет `status: "stale"` в теле
        символа (идемпотентно). Живые символы получают `status: "alive"`.
        Удаление остаётся опцией (purge/refresh с удалением).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        live = set(live_keys or [])
        marked, unmarked = 0, 0
        with self._begin_write() as txn:
            syms = self.dbi(txn, "symbols")
            with txn.cursor(syms) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if (pre and not key.startswith(pre)) or \
                       (not pre and _LAYER_KEY_RE.match(key)):
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    bare = key[len(pre):] if pre else key
                    body = json.loads(v)
                    wanted = "alive" if bare in live else "stale"
                    if body.get("status") != wanted:
                        body["status"] = wanted
                        txn.put(syms, k,
                                json.dumps(body, ensure_ascii=False).encode())
                        if wanted == "stale":
                            marked += 1
                        else:
                            unmarked += 1
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return {"marked_stale": marked, "marked_alive": unmarked}

    def map_generations(self) -> dict:
        """Текущее поколение карты: canary.y (инкрементируется map_load_batch
        при replace) + счётчики символов/рёбер."""
        with self.env.begin(readonly=True) as txn:
            syms = self.dbi(txn, "symbols")
            n_syms = 0
            with txn.cursor(syms) as cur:
                rc, _, _ = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    n_syms += 1
                    rc, _, _ = cur.get(mdbx.CURSOR_NEXT)
            canary = txn.canary_get()  # в той же txn (без вложенной)
            return {"generation": canary.get("y", 0), "symbols": n_syms}

    def map_diff(self, other=None) -> dict:
        """Разница текущего снимка карты с `other` (генерацией или списком).

        Если `other` — dict вида {"generation": G, "symbols": [keys],
        "content_hash": H} от более раннего снимка — вернёт
        добавленные/удалённые/общие символы. `content_hash` используется как
        быстрый путь: совпадение хэша означает идентичное содержимое.
        """
        snap = self.map_snapshot()
        if isinstance(other, dict) and other.get("content_hash") \
                and other["content_hash"] == snap["content_hash"]:
            return {"added": [], "removed": [], "common": snap["symbols"],
                    "added_count": 0, "removed_count": 0,
                    "identical": True}
        cur_keys = set(snap["symbols"])
        if isinstance(other, dict):
            # []/None для пустого снимка — валидное «ничего», не falsy-замена
            keys = other.get("symbols")
            other_keys = set(keys) if keys is not None else cur_keys
        elif isinstance(other, (list, set, tuple)):
            other_keys = set(other)
        else:
            other_keys = set()
        added = sorted(cur_keys - other_keys)
        removed = sorted(other_keys - cur_keys)
        common = sorted(cur_keys & other_keys)
        return {"added": added, "removed": removed, "common": common,
                "added_count": len(added), "removed_count": len(removed)}

    def map_port_status(self, module: str = "", layer: str = "") -> dict:
        """Приборная панель миграции C→C++ по слою.

        Классифицирует функции по курируемому полю `cpp_status`
        (none|mapped|partial|done) + наличию `cpp_eq`. Возвращает агрегат
        по модулям + топ-N крупнейших неспортированных (по числу блоков) —
        «что переписывать следующим».
        """
        mods = {}
        not_ported = []
        # ОДНА read-txn на весь метод (B1): слой резолвится внутри неё —
        # снимок согласован даже при параллельной записи/активации слоя.
        with self.env.begin(readonly=True) as txn:
            layer = self._effective_layer_locked(txn, layer)
            pre = self.layer_prefix(layer)
            for key in self._map_symbols_locked(txn, pre, "fn:"):
                body = self._map_symbol_locked(txn, pre, key)
                mod_list = [i.get("module")
                            for i in body.get("implementations") or []
                            if i.get("module")]
                if body.get("module"):
                    mod_list.insert(0, body.get("module", ""))
                mod = mod_list[0] if mod_list else "?"
                if module and not mod.startswith(module):
                    continue
                st = body.get("cpp_status") or \
                    ("done" if body.get("cpp_eq") else "none")
                if st not in CPP_STATUSES:
                    st = "none"
                m = mods.setdefault(mod, {"total": 0, "none": 0, "mapped": 0,
                                          "partial": 0, "done": 0})
                m["total"] += 1
                m[st] += 1
                if st in ("none", "partial"):
                    blocks = len(body.get("blocks") or [])
                    for impl in body.get("implementations") or []:
                        blocks += len(impl.get("blocks") or [])
                    not_ported.append({"symbol": key, "module": mod,
                                       "status": st, "blocks": blocks})
        not_ported.sort(key=lambda r: -r["blocks"])
        total = {"total": 0, "none": 0, "mapped": 0, "partial": 0, "done": 0}
        for m in mods.values():
            for k in total:
                total[k] += m[k]
        total["pct_done"] = round(100.0 * total["done"] / total["total"], 1) \
            if total["total"] else 0.0
        return {"layer": layer, "module": module, "modules": mods,
                "total": total, "top_not_ported": not_ported[:20]}

    def map_snapshot(self, layer: str = "") -> dict:
        """Атомарный снимок карты: поколение + ключи + content_hash.

        Всё читается в ОДНОЙ read-txn (canary и symbols), поэтому снимок
        согласован даже при параллельной перегенерации карты (C-1):
        «новая генерация + старое содержимое» больше невозможно.

        content_hash — sha1 отсортированных структурных атрибутов символов
        (key, l0, l1, signature, число блоков/implementations): позволяет
        map_diff сравнивать снимки одним сравнением строк.
        """
        keys = []
        parts = []
        with self.env.begin(readonly=True) as txn:
            layer = self._effective_layer_locked(txn, layer)
            pre = self.layer_prefix(layer)
            canary = txn.canary_get()
            generation = canary.get("y", 0)
            sym_dbi = self.dbi(txn, "symbols")
            with txn.cursor(sym_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    if (not pre and _LAYER_KEY_RE.match(key)) or \
                       (pre and not key.startswith(pre)):
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    bare = key[len(pre):] if pre else key
                    keys.append(bare)
                    try:
                        body = json.loads(v)
                    except ValueError:
                        body = {}
                    parts.append("%s\x00%s\x00%s\x00%s\x00%d\x00%d" % (
                        bare, body.get("l0"), body.get("l1"),
                        body.get("signature", ""),
                        len(body.get("blocks") or []),
                        len(body.get("implementations") or [])))
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        keys.sort()
        parts.sort()
        content_hash = hashlib.sha1("\n".join(parts).encode()).hexdigest()
        return {"generation": generation, "layer": layer,
                "symbols": keys, "content_hash": content_hash}

    def map_diff_branches(self, from_layer: str, to_layer: str,
                          limit: int = 200) -> dict:
        """Структурный diff двух слоёв веток ПО СОДЕРЖИМОМУ карты.

        Сравнивает множество символов (fn:/type:/macro:) и диапазоны
        (l0/l1), а не коммиты: это честный ответ на «масштаб различий»,
        не зависящий от переписывания истории.

        Возвращает: добавленные/удалённые/изменённые символы (по модулям),
        число затронутых функций/блоков, и совпавшие (common).
        """
        def _structural(body):
            """Нормализованное структурное содержимое для сравнения.

            Учитывает и nested-блоки внутри implementations (merged-карты
            хранят blocks там, а не на верхнем card).
            """
            return (body.get("l0"), body.get("l1"),
                    body.get("signature"), body.get("file"),
                    tuple((bl.get("id"), bl.get("l0"), bl.get("l1"))
                          for bl in body.get("blocks") or []),
                    tuple(sorted((i.get("file"), i.get("l0"), i.get("l1"),
                                  i.get("config"),
                                  tuple((b.get("id"), b.get("l0"), b.get("l1"))
                                        for b in i.get("blocks") or []))
                                 for i in body.get("implementations") or [])))

        def _hash(body) -> str:
            """Стабильный хэш структурного содержимого символа (B2): diff
            сравнивает хэши, а не полные тела — оба слоя не держатся в
            памяти одновременно."""
            return hashlib.sha1(repr(_structural(body)).encode()).hexdigest()

        # ОДНА read-txn на оба слоя (B1) + hash-based сравнение (B2):
        # один проход собирает хэши+meta обоих слоёв и полные тела to-слоя;
        # полные тела from-слоя подгружаем ТОЛЬКО для removed-ключей (в той
        # же txn). Пик памяти ~ размер to-слоя + дельты, а не оба слоя целиком.
        with self.env.begin(readonly=True) as txn:
            pre_a = self.layer_prefix(from_layer)
            pre_b = self.layer_prefix(to_layer)
            a_hash, a_meta = {}, {}
            b_hash, b_body = {}, {}
            sym_dbi = self.dbi(txn, "symbols")
            with txn.cursor(sym_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    for pre, acc_h, acc_m, keep_body in (
                            (pre_a, a_hash, a_meta, False),
                            (pre_b, b_hash, b_body, True)):
                        # базовый слой (pre=="") не захватывает чужие feature
                        if not pre and _LAYER_KEY_RE.match(key):
                            continue
                        if pre and not key.startswith(pre):
                            continue
                        bare = key[len(pre):] if pre else key
                        try:
                            body = json.loads(v)
                        except ValueError:
                            break  # повреждённая запись: пропустить целиком
                        acc_h[bare] = _hash(body)
                        if keep_body:
                            acc_m[bare] = body
                        else:
                            acc_m[bare] = {
                                "module": body.get("module"),
                                "implementations": [
                                    {"module": i.get("module")}
                                    for i in body.get("implementations") or []
                                ] if body.get("implementations") else [],
                            }
                        # НЕ break: ключ может совпасть с ОБОИМИ префиксами
                        # (same-layer/same-prefix diff, pre_a == pre_b) —
                        # тогда он принадлежит обеим сторонам сравнения.
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            # различия по хэшам; полные тела added/changed уже есть (b_body);
            # для removed дочитываем from-тела точечно (их немного).
            added = {}
            removed = {}
            changed = {}
            for key, h in b_hash.items():
                if key not in a_hash:
                    added[key] = b_body[key]
                elif a_hash[key] != h:
                    changed[key] = {"from": a_meta[key], "to": b_body[key]}
            for key in a_hash:
                if key not in b_hash:
                    removed[key] = self._map_symbol_locked(txn, pre_a, key)

        def by_module(d):
            m = {}
            for key, body in d.items():
                # merged-карты: module внутри implementations
                mods = [i.get("module") for i in body.get("implementations") or []
                        if i.get("module")]
                mod = mods[0] if mods else body.get("module", "?")
                m.setdefault(mod, []).append(key)
            for k in m:
                m[k].sort()
            return m

        def _block_count(body):
            n = len(body.get("blocks") or [])
            for i in body.get("implementations") or []:
                n += len(i.get("blocks") or [])
            return n

        def _nf(d):
            return sum(1 for b in d.values() if b.get("kind") == "function")

        def _nb(d):
            return sum(_block_count(b) for b in d.values())
        return {
            "from": from_layer, "to": to_layer,
            "added": by_module(added),
            "removed": by_module(removed),
            "changed": by_module({k: v["to"] for k, v in changed.items()}),
            "counts": {
                "added": len(added), "removed": len(removed),
                "changed": len(changed),
                "added_functions": _nf(added),
                "removed_functions": _nf(removed),
                "changed_functions": _nf({k: v["to"] for k, v in changed.items()}),
                "added_blocks": _nb(added),
                "removed_blocks": _nb(removed),
            },
            "total_from": len(a_hash), "total_to": len(b_hash),
        }

    # --- regions / uncovered (refactoring-map: #if-дерево и острова смысла) ---
    def map_regions(self, prefix: str = "", limit: int = None) -> list:
        """Сканирование таблицы regions (region:{module}:{n})."""
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self.dbi(txn, "regions")
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    if key.startswith(prefix):
                        out.append({"key": key,
                                    "region": json.loads(v)})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def map_region(self, key: str) -> dict:
        """Один регион #if-дерева."""
        with self.env.begin(readonly=True) as txn:
            rc, v = txn.get(self.dbi(txn, "regions"), key.encode())
            if rc != mdbx.RC_SUCCESS or v is None:
                return {}
            return json.loads(v)

    def map_uncovered(self, prefix: str = "", limit: int = None) -> list:
        """Сканирование uncovered-островов (uncovered:{module}:{n})."""
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self.dbi(txn, "uncovered")
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    if key.startswith("uncovered:" + prefix):
                        out.append({"key": key,
                                    "uncovered": json.loads(v)})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    @staticmethod
    def _module_of_file(file: str) -> str:
        """Эвристика модуля по пути: src/<mod>.c -> <mod>;
        tests/<area>/... -> tests-<area>; иначе базовое имя без расширения."""
        f = file.replace("\\", "/")
        parts = [p for p in f.split("/") if p]
        base = os.path.splitext(parts[-1])[0] if parts else "?"
        if len(parts) >= 2 and parts[0] == "src":
            return base
        if len(parts) >= 3 and parts[0] == "tests":
            return "tests-" + parts[1]
        return base

    @staticmethod
    def _uncovered_key(file: str, idx: int) -> str:
        return "uncovered:%s:%d" % (Store._module_of_file(file), idx)

    # --- слой карты проекта (весь репозиторий: cmake/scripts/docs/ci) --------
    def project_load(self, files: dict, edges: list, replace: bool = True) -> dict:
        """Загружает project-map: файлы и рёбра file_deps.

        `files`: rel_path -> {kind, lang, module, size?, deps?}
        `edges`: [{from, to, kind}] — дедуплицируются в file:from -> [to...].
        """
        with self._begin_write() as txn:
            if replace:
                for dbi in ("project_files", "file_deps"):
                    with txn.cursor(self.dbi(txn, dbi)) as cur:
                        rc, _, _ = cur.get(mdbx.CURSOR_FIRST)
                        while rc == mdbx.RC_SUCCESS:
                            cur.delete()
                            rc, _, _ = cur.get(mdbx.CURSOR_NEXT)
            pf_dbi = self.dbi(txn, "project_files")
            fd_dbi = self.dbi(txn, "file_deps")
            for rel, meta in sorted(files.items()):
                body = {k: v for k, v in meta.items() if k != "deps"}
                txn.put(pf_dbi, rel.encode(),
                        json.dumps(body, ensure_ascii=False).encode())
            # агрегируем рёбра: file:from -> set(to)
            dep_map = {}
            for e in edges:
                src = e.get("from")
                dst = e.get("to")
                if not src or not dst:
                    continue
                dep_map.setdefault(src, set()).add(dst)
            for src, dsts in sorted(dep_map.items()):
                txn.put(fd_dbi, ("file:" + src).encode(),
                        json.dumps(sorted(dsts), ensure_ascii=False).encode())
            return {"files": len(files), "edges": len(edges)}

    def project_files(self, prefix: str = "", limit: int = None) -> list:
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "project_files")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if key.startswith(prefix):
                        body = json.loads(v)
                        out.append({"path": key, "meta": body})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def file_deps(self, path: str) -> list:
        """Зависимости файла (file:path -> [dep...])."""
        with self.env.begin(readonly=True) as txn:
            fd_dbi = self._opt_dbi(txn, "file_deps")
            if fd_dbi is None:
                return []
            rc, val = txn.get(fd_dbi, ("file:" + path).encode())
            if val is None:
                return []
            return json.loads(val.decode())

    def file_dependents(self, path: str, limit: int = 50) -> list:
        """Файлы, зависящие от path (обратный обход file_deps)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "file_deps")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    src = k.decode("utf-8", "replace")[len("file:"):]
                    if path in (json.loads(v) or []):
                        out.append(src)
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    # --- реестр probes / SystemMap -------------------------------------------
    def probe_add(self, tag: str, kind: str = None, mechanism: str = "",
                  test_theme: str = "", symbol: str = "", file: str = "",
                  line: int = 0, status: str = None,
                  layer: str = "") -> dict:
        """Регистрирует зонд/пробу (SystemMap).

        `kind`: usdt | bookmark | injected | dev_assert
        `status`: candidate | inserted | validated
        При обновлении существующего зонда `kind`/`status`, переданные как
        None (не указаны), сохраняют старые значения; дефолты (usdt /
        candidate) применяются ТОЛЬКО при создании — чтобы probe_status не
        сбрасывал kind, а обновление локации не откатывало status.
        Тег — семантический (стабильный при рефакторинге). Один тег → одна
        запись (dedup по тегу), как в probe-bus (§4 «Реестр»).
        """
        tag = tag.strip()
        if not tag:
            raise invalid("probe: пустой тег", "укажите семантический тег")
        if kind is not None and kind not in PROBE_KINDS:
            raise invalid(
                "probe: неизвестный kind %r" % kind,
                "допустимые: %s" % ", ".join(sorted(PROBE_KINDS)))
        if status is not None and status not in PROBE_STATUSES:
            raise invalid(
                "probe: неизвестный status %r" % status,
                "допустимые: %s" % ", ".join(sorted(PROBE_STATUSES)))
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "probes")
            key = ("probe:" + pre + tag).encode()
            rc, old_val = txn.get(dbi, key)
            old = json.loads(old_val.decode()) if old_val else {}
            is_new = not old_val
            body = {
                "tag": tag,
                "kind": (kind if kind is not None
                         else (old.get("kind") if not is_new else "usdt")),
                "mechanism": mechanism or old.get("mechanism", ""),
                "test_theme": test_theme or old.get("test_theme", ""),
                "symbol": symbol or old.get("symbol", ""),
                "file": file or old.get("file", ""),
                "line": line or old.get("line", 0),
                "status": (status if status is not None
                           else (old.get("status") if not is_new else "candidate")),
            }
            txn.put(dbi, key, json.dumps(body, ensure_ascii=False).encode())
            return {"result": "merged" if old_val else "created", "tag": tag}

    def probe_batch(self, probes: list, layer: str = "") -> dict:
        """Батчевая регистрация зондов (из scan_probes.py).

        Невалидные записи (пустой тег, неизвестный kind/status) ПРОПУСКАЮТСЯ
        с отчётом в `skipped` — одна бракованная строка не роняет загрузку
        всего реестра (решение A5-β: skip+отчёт, не fail-fast).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        n = 0
        skipped = []
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "probes")
            for p in probes:
                tag = (p.get("tag") or "").strip()
                if not tag:
                    skipped.append({"reason": "empty-tag", "entry": str(p)[:80]})
                    continue
                kind = p.get("kind")
                if kind is not None and kind not in PROBE_KINDS:
                    skipped.append({"reason": "bad-kind:%s" % kind, "tag": tag})
                    continue
                status = p.get("status")
                if status is not None and status not in PROBE_STATUSES:
                    skipped.append({"reason": "bad-status:%s" % status,
                                    "tag": tag})
                    continue
                key = ("probe:" + pre + tag).encode()
                rc, old_val = txn.get(dbi, key)
                old = json.loads(old_val.decode()) if old_val else {}
                is_new = not old_val
                body = {"tag": tag,
                        "kind": (kind if kind is not None else
                                 (old.get("kind") if not is_new else "usdt")),
                        "mechanism": p.get("mechanism") or old.get("mechanism", ""),
                        "test_theme": p.get("test_theme") or old.get("test_theme", ""),
                        "symbol": p.get("symbol") or old.get("symbol", ""),
                        "file": p.get("file") or old.get("file", ""),
                        "line": int(p.get("line") or old.get("line", 0)),
                        "status": (status if status is not None else
                                   (old.get("status") if not is_new
                                    else "candidate"))}
                txn.put(dbi, key, json.dumps(body, ensure_ascii=False).encode())
                n += 1
            return {"probes": n, "skipped": skipped}

    def probe_get(self, tag: str, layer: str = "") -> dict:
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "probes")
            if dbi is None:
                return {}
            rc, val = txn.get(dbi, ("probe:" + pre + tag).encode())
            if val is None:
                return {}
            return json.loads(val.decode())

    def probe_list(self, prefix: str = "", kind: str = "",
                   limit: int = None, layer: str = "") -> list:
        """Зонды слоя. `prefix` применяется к тегу (после layer-префикса)."""
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "probes")
            if dbi is None:
                return []  # старая БД без probes
            scan_pre = "probe:" + pre
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(scan_pre):
                        break
                    tag = key[len(scan_pre):]
                    # базовый слой не видит чужие feature-зонды
                    if not pre and _LAYER_KEY_RE.match(tag):
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    if tag.startswith(prefix):
                        body = json.loads(v)
                        if not kind or body.get("kind") == kind:
                            out.append({"tag": tag, "body": body})
                            if limit and len(out) >= limit:
                                break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def probe_status(self, tag: str, status: str, layer: str = "") -> dict:
        """Переводит статус зонда (candidate/inserted/validated).

        `status` валидируется по PROBE_STATUSES (C-8): произвольные строки
        отвергаются как invalid.
        """
        return self.probe_add(tag, status=status, layer=layer)

    def probe_remove(self, tag: str, layer: str = "") -> dict:
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "probes")
            key = ("probe:" + pre + tag).encode()
            rc, val = txn.get(dbi, key)
            if val is None:
                return {"result": "notfound"}
            txn.delete(dbi, key)
            return {"result": "removed"}

    # --- наблюдаемые вызовы (runtime traces) ---------------------------------
    @staticmethod
    def _trace_count(m: dict, callee: str) -> int:
        """Счётчик callee из агрегата (совместимо со старым int-форматом)."""
        v = m.get(callee)
        if isinstance(v, dict):
            return int(v.get("count", 0))
        return int(v or 0)

    def trace_ingest(self, caller: str, callee: str, count: int = 1,
                     label: str = "", layer: str = "") -> dict:
        """Фиксирует наблюдаемый вызов (possible vs observed).

        Хранит одну запись на caller: `{callee: {count, label}}` (старый
        формат `{callee: int}` принимается при чтении). `label` — метка
        источника трассы (напр. "ut-run", "probe-bus"); повторный ingest с
        другим label перезаписывает label (последний побеждает, A5-α),
        count накапливается. Обратные рёбра — put_nodupe (без дублей, C-6).
        """
        count = max(1, int(count))
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            ct_dbi = self.dbi(txn, "call_traces")
            ctr_dbi = self.dbi(txn, "call_traces_rev")
            key = (pre + caller).encode()
            rc, val = txn.get(ct_dbi, key)
            m = json.loads(val.decode()) if val else {}
            prev = self._trace_count(m, callee)
            entry = {"count": prev + count}
            if label:
                entry["label"] = label
            elif isinstance(m.get(callee), dict) and m[callee].get("label"):
                entry["label"] = m[callee]["label"]  # сохраняем прежний
            m[callee] = entry
            txn.put(ct_dbi, key, json.dumps(m, ensure_ascii=False).encode())
            with txn.cursor(ctr_dbi) as cur:
                cur.put_nodupe((pre + callee).encode(),
                               (pre + caller).encode())
            return {"caller": caller, "callee": callee,
                    "count": entry["count"], "layer": layer}

    def trace_ingest_batch(self, pairs: list, layer: str = "") -> dict:
        """Батчевый ingest наблюдаемых вызовов в одной write-txn.

        pairs: [{"caller", "callee", "count"?, "label"?}] — из дампов
        probe-bus/счётчиков (collect_traces.py). Быстрее цикла одиночных
        trace_ingest (одна txn на файл, B3-δ).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        n = 0
        with self._begin_write() as txn:
            ct_dbi = self.dbi(txn, "call_traces")
            ctr_dbi = self.dbi(txn, "call_traces_rev")
            for p in pairs:
                caller = (p.get("caller") or "").strip()
                callee = (p.get("callee") or "").strip()
                if not caller or not callee:
                    continue
                count = max(1, int(p.get("count", 1)))
                label = p.get("label", "")
                key = (pre + caller).encode()
                rc, val = txn.get(ct_dbi, key)
                m = json.loads(val.decode()) if val else {}
                prev = self._trace_count(m, callee)
                entry = {"count": prev + count}
                if label:
                    entry["label"] = label
                elif isinstance(m.get(callee), dict) and m[callee].get("label"):
                    entry["label"] = m[callee]["label"]
                m[callee] = entry
                txn.put(ct_dbi, key, json.dumps(m, ensure_ascii=False).encode())
                with txn.cursor(ctr_dbi) as cur:
                    cur.put_nodupe((pre + callee).encode(),
                                   (pre + caller).encode())
                n += 1
        return {"ingested": n, "layer": layer}

    def trace_observed(self, caller: str = "", layer: str = "") -> list:
        """Наблюдаемые вызовы слоя (по префиксу caller, layer-local ключи)."""
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "call_traces")
            if dbi is None:
                return []
            scan_pre = pre + caller
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(scan_pre):
                        break
                    bare_caller = key[len(pre):] if pre else key
                    body = json.loads(v)
                    for cal in sorted(body.keys()):
                        cnt = self._trace_count(body, cal)
                        rec = {"caller": bare_caller, "callee": cal,
                               "count": cnt}
                        if isinstance(body.get(cal), dict) and \
                                body[cal].get("label"):
                            rec["label"] = body[cal]["label"]
                        out.append(rec)
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def trace_path(self, src: str, dst: str, depth: int = 5,
                   limit: int = 50, layer: str = "") -> list:
        """BFS по наблюдаемым рёбрам слоя: src → dst (layer-local ключи).

        Ленивый BFS (B2): соседи читаются точечными get из одной read-txn
        по мере обхода, а не полным снимком adj всей таблицы в память —
        при больших трассах это разница между O(путей) и O(всего слоя).
        """
        from collections import deque
        found = []
        with self.env.begin(readonly=True) as txn:
            layer = self._effective_layer_locked(txn, layer)
            pre = self.layer_prefix(layer)
            dbi = self._opt_dbi(txn, "call_traces")
            if dbi is None:
                return []

            def neighbors(node: str) -> list:
                rc, val = txn.get(dbi, (pre + node).encode())
                if val is None:
                    return []
                try:
                    return list(json.loads(val.decode()).keys())
                except ValueError:
                    return []

            seen = {(src,)}
            queue = deque([(src, [src])])
            while queue and len(found) < limit:
                node, path = queue.popleft()
                if len(path) > depth:
                    continue
                for nxt in neighbors(node):
                    if not nxt or nxt in path:
                        continue
                    new_path = path + [nxt]
                    if nxt == dst:
                        found.append(new_path)
                        if len(found) >= limit:
                            break
                    if len(new_path) <= depth and tuple(new_path) not in seen:
                        seen.add(tuple(new_path))
                        queue.append((nxt, new_path))
        return found

    def trace_compare(self, symbol: str, layer: str = "") -> dict:
        """Сравнение static (call_edges) и observed (call_traces) для символа.

        observed берётся ТОЧНО по caller (не prefix), чтобы fn:foo не
        включало наблюдения из fn:foobar.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        static_callees = set()
        with self.env.begin(readonly=True) as txn:
            ce_dbi = self.dbi(txn, "call_edges")
            with txn.cursor(ce_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY,
                                     (pre + symbol).encode())
                while rc == mdbx.RC_SUCCESS:
                    try:
                        body = json.loads(val.decode())
                        # только syntax-рёбра: semantic-саморебро (эквивалентные
                        # конфиги) не является реальным вызовом
                        if body.get("kind") != "syntax":
                            rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
                            continue
                        cal = body.get("callee", "")
                        if pre and cal.startswith(pre):
                            cal = cal[len(pre):]
                        static_callees.add(cal)
                    except ValueError:
                        # повреждённое ребро: пропустить (перегенерируется)
                        rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
                        continue
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        observed_callees = set()
        with self.env.begin(readonly=True) as txn:
            ct_dbi = self._opt_dbi(txn, "call_traces")
            if ct_dbi is not None:
                rc, val = txn.get(ct_dbi, (pre + symbol).encode())
                if val is not None:
                    for cal in json.loads(val.decode()).keys():
                        # нормализуем observed до layer-local (префикс слоя снят)
                        if pre and cal.startswith(pre):
                            cal = cal[len(pre):]
                        observed_callees.add(cal)
        return {
            "symbol": symbol,
            "static": sorted(static_callees),
            "observed": sorted(observed_callees),
            "only_static": sorted(static_callees - observed_callees),
            "only_observed": sorted(observed_callees - static_callees),
        }

    # --- ADR (архитектурные решения) -----------------------------------------
    def adr_update(self, name: str, body: dict) -> dict:
        """Сохраняет/обновляет ADR (context/decision/consequences).

        Формат body (секции ADR): {"title", "status", "date", "context",
        "decision", "consequences", "sections": {<имя>: <текст>}}. Любые
        секции допускаются; обновление заменяет только переданные поля.
        """
        name = name.strip()
        if not name:
            raise invalid("adr: пустое имя", "укажите имя ADR")
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "adr")
            key = ("adr:" + name).encode()
            rc, old_val = txn.get(dbi, key)
            old = json.loads(old_val.decode()) if old_val else {}
            sections = dict(old.get("sections") or {})
            if body.get("sections"):
                sections.update(body["sections"])
            new = {
                "title": body.get("title", old.get("title", name)),
                "status": body.get("status", old.get("status", "draft")),
                "date": body.get("date", old.get("date", "")),
                "context": body.get("context", old.get("context", "")),
                "decision": body.get("decision", old.get("decision", "")),
                "consequences": body.get("consequences",
                                         old.get("consequences", "")),
                "sections": sections,
            }
            txn.put(dbi, key, json.dumps(new, ensure_ascii=False).encode())
            return {"result": "merged" if old_val else "created", "name": name}

    def adr_get(self, name: str) -> dict:
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "adr")
            if dbi is None:
                return {}
            rc, val = txn.get(dbi, ("adr:" + name).encode())
            if val is None:
                return {}
            return json.loads(val.decode())

    def adr_outline(self, prefix: str = "", limit: int = None) -> list:
        """Заголовки всех ADR (title/status/date) — краткий обзор."""
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "adr")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if key.startswith("adr:" + prefix):
                        body = json.loads(v)
                        out.append({"name": key[len("adr:"):],
                                    "title": body.get("title", ""),
                                    "status": body.get("status", ""),
                                    "date": body.get("date", "")})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def adr_delete(self, name: str) -> dict:
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "adr")
            key = ("adr:" + name).encode()
            rc, val = txn.get(dbi, key)
            if val is None:
                return {"result": "notfound"}
            txn.delete(dbi, key)
            return {"result": "removed"}

    # --- мост с codebase-memory-mcp (опциональный, совместный режим) ---------
    @staticmethod
    def cbm_binary() -> str:
        """Путь к бинарю codebase-memory-mcp (или "" если не установлен)."""
        env = os.environ.get("CBM_BIN")
        if env:
            return env if os.path.exists(env) else ""
        for cand in (os.path.expanduser("~/.local/bin/codebase-memory-mcp"),
                     "/usr/local/bin/codebase-memory-mcp"):
            if os.path.exists(cand):
                return cand
        import shutil
        return shutil.which("codebase-memory-mcp") or ""

    def cbm_link(self, our_key: str, cbm_node: str,
                 cbm_project: str = "") -> dict:
        """Записывает соответствие наш fq-ключ ↔ узел CBM."""
        if not our_key:
            raise invalid("cbm: пустой our_key", "укажите fq-ключ")
        with self._begin_write() as txn:
            body = {"our_key": our_key, "cbm_node": cbm_node,
                    "cbm_project": cbm_project,
                    "mapped_at": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                               time.gmtime())}
            txn.put(self.dbi(txn, "bridge"),
                    ("bridge:" + our_key).encode(),
                    json.dumps(body, ensure_ascii=False).encode())
            return body

    def cbm_unlink(self, our_key: str) -> dict:
        with self._begin_write() as txn:
            key = ("bridge:" + our_key).encode()
            rc, val = txn.get(self.dbi(txn, "bridge"), key)
            if val is None:
                return {"result": "notfound"}
            txn.delete(self.dbi(txn, "bridge"), key)
            return {"result": "removed"}

    def cbm_linked(self, prefix: str = "", limit: int = 50) -> list:
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "bridge")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if key.startswith("bridge:" + prefix):
                        out.append(json.loads(v))
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def cbm_search(self, query: str, project: str = "",
                   tool: str = "search_graph", args: dict = None,
                   timeout: int = 60) -> dict:
        """Вызывает CBM CLI (подпроцесс) для query.

        Основной режим — совместная работа с codebase-memory-mcp; НО модуль
        полноценно работает и без CBM: если бинарь не найден или вызов
        падает — возвращается {"unavailable": true, ...}, не ошибка.
        """
        binary = self.cbm_binary()
        if not binary:
            return {"unavailable": True,
                    "reason": "codebase-memory-mcp not installed"}
        import subprocess as _sp
        args = dict(args or {})
        if tool == "search_graph":
            args.setdefault("query", query)
        elif tool == "search_code":
            args.setdefault("pattern", query)
        if project:
            args.setdefault("project", project)
        cmd = [binary, "cli", "--json", tool]
        # передаём аргументы как JSON строку после tool? CBM cli принимает
        # args в JSON-форме позиционно
        import json as _json
        cmd.append(_json.dumps(args, ensure_ascii=False))
        try:
            p = _sp.run(cmd, capture_output=True, text=True,
                        timeout=timeout, check=False)
        except Exception as e:  # noqa: BLE001
            return {"unavailable": True, "reason": "cbm cli error: %s" % e}
        if p.returncode != 0:
            return {"unavailable": True,
                    "reason": "cbm cli rc=%s: %s" % (p.returncode,
                                                     p.stderr[:300])}
        try:
            envelope = json.loads(p.stdout)
        except ValueError:
            return {"raw": p.stdout[:2000], "reason": "non-JSON output"}
        return {"cbm": True, "tool": tool, "query": query,
                "result": envelope}

    def map_load_regions(self, regions: list, uncovered: list,
                         replace: bool = False) -> dict:
        """Пакетная загрузка regions + uncovered (из артефакта refactoring-map).

        Ключи: region:{module}:{n} (id из артефакта) / uncovered:{module}:{n}.
        При replace=True таблицы очищаются перед загрузкой.
        """
        with self._begin_write() as txn:
            reg_dbi = self.dbi(txn, "regions")
            unc_dbi = self.dbi(txn, "uncovered")
            if replace:
                for dbi in (reg_dbi, unc_dbi):
                    with txn.cursor(dbi) as cur:
                        rc, _, _ = cur.get(mdbx.CURSOR_FIRST)
                        while rc == mdbx.RC_SUCCESS:
                            cur.delete()
                            rc, _, _ = cur.get(mdbx.CURSOR_NEXT)
            for r in regions:
                rid = r.get("id") or ("region:%s:%d" % (
                    self._module_of_file(r.get("file", "")), r.get("l0", 0)))
                txn.put(reg_dbi, rid.encode(), json.dumps(r).encode())
            for i, u in enumerate(uncovered):
                key = self._uncovered_key(u.get("file", ""), i)
                txn.put(unc_dbi, key.encode(), json.dumps(u).encode())
        return {"regions": len(regions), "uncovered": len(uncovered)}

    @staticmethod
    def _alias_target_locked(txn, al_dbi, key):
        """Цель алиаса (разрешение цепочек) в рамках текущей txn."""
        seen, cur = set(), key
        while cur and cur not in seen:
            seen.add(cur)
            rc, val = txn.get(al_dbi, cur.encode())
            if rc != mdbx.RC_SUCCESS or val is None:
                break
            cur = val.decode()
        return cur if cur != key else ""

    def _sym_id_locked(self, txn, key):
        """Стабильный id символа в рамках текущей txn.

        Выделяется из общего счётчика next_id (как записи знаний) —
        единое числовое пространство для links.
        """
        sid_dbi = self.dbi(txn, "sym_ids")
        rc, val = txn.get(sid_dbi, key.encode())
        if val is not None and len(val) == 8:
            return _unpack_u64(val)
        nid = self._alloc_id_locked(txn)
        txn.put(sid_dbi, key.encode(), _pack_u64(nid))
        txn.put(self.dbi(txn, "sym_id2key"), _pack_u64(nid), key.encode())
        return nid

    # --- слой тестов/CI: test_metrics / coverage / ci_jobs -------------------
    def test_metrics_insert(self, test_id: str, duration_ms: float,
                            labels: list = None, timeout_sec: int = 0,
                            window: int = 16) -> dict:
        """Обновляет окно истории длительностей тест-кейса (additive).

        Хранит не чистый снапшот, а скользящее окно: last/min/max/p90,
        число прогонов и счётчик флаков (runs > 1 и резкое удлинение).
        Наполняется коллектором ``tools/collect_test_metrics.py`` из ctest XML.
        """
        with self._begin_write() as txn:
            return self._test_metrics_put_locked(
                txn, test_id, duration_ms, labels, timeout_sec, window)

    def _test_metrics_put_locked(self, txn, test_id: str, duration_ms: float,
                                 labels: list = None, timeout_sec: int = 0,
                                 window: int = 16) -> dict:
        """RMW окна истории одного теста внутри write-txn (общая точка)."""
        key = "test_metrics:" + test_id
        rec_dbi = self.dbi(txn, "test_metrics")
        rc, old_val = txn.get(rec_dbi, key.encode())
        old = json.loads(old_val.decode()) if old_val else {}
        runs = old.get("runs", 0)
        vals = list(old.get("history", []))[-window:]
        old_max = max(vals) if vals else 0.0
        vals.append(duration_ms)
        vals = vals[-window:]
        last = vals[-1]
        mn = min(vals)
        mx = max(vals)
        sorted_v = sorted(vals)
        p90 = sorted_v[int(len(sorted_v) * 0.9) - 1] if vals else 0.0
        flaky = old.get("flaky_count", 0)
        # резкое удлинение против истории (> 2x старого max при min 3 прогона)
        if len(vals) >= 4 and old_max > 0 and last > max(2.0 * old_max, 10.0):
            flaky += 1
        body = {
            "last_ms": round(last, 1),
            "min_ms": round(mn, 1),
            "max_ms": round(mx, 1),
            "p90_ms": round(p90, 1),
            "runs": runs + 1,
            "flaky_count": flaky,
            "timeout_sec": timeout_sec,
            "labels": labels or old.get("labels", []),
            "history": vals,
        }
        txn.put(rec_dbi, key.encode(),
                json.dumps(body, ensure_ascii=False).encode())
        return {"test": test_id, "body": body}

    def test_metrics_batch(self, metrics: dict, window: int = 16) -> dict:
        """Батчевая запись метрик: {test_id: {...}} в ОДНОЙ write-txn (A3).

        Каждый элемент — как у test_metrics_insert (duration_ms, labels?,
        timeout_sec?). Одна txn на ctest-прогон вместо txn на тест —
        collect_test_metrics не удерживает writer-лок роя между тестами.
        """
        n = 0
        with self._begin_write() as txn:
            for test_id, info in (metrics or {}).items():
                info = info or {}
                self._test_metrics_put_locked(
                    txn, test_id, float(info.get("duration_ms", 0.0)),
                    info.get("labels"), int(info.get("timeout_sec", 0) or 0),
                    window)
                n += 1
        return {"tests": n}

    def test_metrics_get(self, test_id: str) -> dict:
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "test_metrics")
            if dbi is None:
                return {}
            rc, val = txn.get(dbi, ("test_metrics:" + test_id).encode())
            if val is None:
                return {}
            return json.loads(val.decode())

    def test_metrics_all(self, prefix: str = "", limit: int = None) -> list:
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "test_metrics")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if key.startswith("test_metrics:" + prefix):
                        out.append({"test": key[len("test_metrics:"):],
                                    "body": json.loads(v)})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def _coverage_put_locked(self, txn, pre: str, test_id: str,
                             symbols: list, pct: float) -> None:
        """Общая запись покрытия теста (внутри write-txn).

        Единая точка для insert/batch: coverage + covered_by (+чистка
        устаревших членств и блочных записей для old−new). Ключи:
        `coverage:{pre}{test}`, `covered_by:{pre}{sym}`,
        `coverage_blocks:{pre}{test}:{sym}`.
        """
        cov_dbi = self.dbi(txn, "coverage")
        cb_dbi = self.dbi(txn, "covered_by")
        cbk_dbi = self.dbi(txn, "coverage_blocks")
        key = ("coverage:" + pre + test_id).encode()
        rc, old_val = txn.get(cov_dbi, key)
        old_syms = set(json.loads(old_val.decode()).get("symbols", [])
                       ) if old_val else set()
        new_syms = set(symbols)
        body = {"test_id": pre + test_id, "symbols": symbols,
                "pct": round(pct, 1)}
        txn.put(cov_dbi, key, json.dumps(body, ensure_ascii=False).encode())
        for sym in symbols:
            cb_key = ("covered_by:" + pre + sym).encode()
            rc, val = txn.get(cb_dbi, cb_key)
            cur = json.loads(val.decode()) if val else []
            if test_id not in cur:
                cur.append(test_id)
                txn.put(cb_dbi, cb_key,
                        json.dumps(cur, ensure_ascii=False).encode())
        # убрать устаревшие членства и блоки (prev - new)
        for sym in old_syms - new_syms:
            cb_key = ("covered_by:" + pre + sym).encode()
            rc, val = txn.get(cb_dbi, cb_key)
            if val is not None:
                cur = json.loads(val.decode())
                if test_id in cur:
                    cur.remove(test_id)
                    if cur:
                        txn.put(cb_dbi, cb_key,
                                json.dumps(cur, ensure_ascii=False).encode())
                    else:
                        txn.delete(cb_dbi, cb_key)
            bk = ("coverage_blocks:%s%s:%s" % (pre, test_id, sym)).encode()
            if txn.get(cbk_dbi, bk)[1] is not None:
                txn.delete(cbk_dbi, bk)

    def coverage_insert(self, test_id: str, symbols: list, pct: float = 0.0,
                        layer: str = "") -> dict:
        """Фиксирует покрытие теста: набор символов (fn:/type:/macro:).

        `layer` — слой ветки (пустой → активная ветка); префиксация ключей
        едина с coverage_blocks/covered_by (одна точка — Store).
        Одновременно поддерживает обратный индекс `covered_by:{sym}` →
        [test_id]. Устаревшие членства и блоки (old−new) удаляются в той же
        транзакции.
        """
        symbols = symbols or []
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            self._coverage_put_locked(txn, pre, test_id, symbols, pct)
            return {"test": pre + test_id, "symbols": len(symbols)}

    def coverage_batch(self, tests: dict, layer: str = "") -> dict:
        """Батчевая фиксация покрытия: {test_id: {symbols, pct}} в одной txn.

        Та же семантика, что coverage_insert (через _coverage_put_locked):
        устаревшие членства covered_by И блочные записи old−new удаляются.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        n = 0
        with self._begin_write() as txn:
            for test_id, info in tests.items():
                symbols = (info or {}).get("symbols") or []
                pct = float((info or {}).get("pct", 0.0))
                self._coverage_put_locked(txn, pre, test_id, symbols, pct)
                n += 1
            return {"tests": n}

    def coverage_migrate(self, from_layer: str, to_layer: str,
                         symbols: list = None) -> dict:
        """Копирует покрытие/блоки/трассы слоя в другой слой (merge ветки).

        N5 (BRANCHES.md §Слияние): при слиянии feature-ветки её прогон
        coverage переносится в целевой слой, чтобы белая карта покрытия
        не начиналась с нуля. Источник НЕ изменяется (копия).

        Переносятся: coverage (+covered_by через _coverage_put_locked),
        coverage_blocks и call_traces(+rev). `symbols` — необязательный
        фильтр; АСИММЕТРИЧЕН: coverage.symbols фильтруются по membership
        (тест, потерявший все символы под фильтром, НЕ переносится —
        целевая запись остаётся нетронутой), coverage_blocks — по symbol
        записи, call_traces — по CALLER (тело копируется целиком, callee
        вне фильтра остаётся в теле как часть наблюдения источника).
        Конфликт test_id/caller в целевом слое — ПЕРЕЗАПИСЬ
        (merge-семантика: свежие данные источника побеждают;
        _coverage_put_locked сам чистит устаревшие членства covered_by,
        висячие call_traces_rev удалённых callee — тоже, P2).
        Битые/несогласованные записи источника пропускаются с отчётом в
        "skipped" (миграция не падает и не порождает мусорных ключей).
        Одна read-txn сбора + одна write-txn записи.
        """
        from_layer = (from_layer or "").strip().strip("/")
        to_layer = (to_layer or "").strip().strip("/")
        pre_from = self.layer_prefix(from_layer)
        pre_to = self.layer_prefix(to_layer)
        if pre_from == pre_to:
            # совпадение имён ИЛИ разные имена с одним префиксом
            # ("0.15" и "0.15.base" оба → ""): миграция бессмысленна
            raise invalid(
                "coverage_migrate: слои %r и %r резолвятся в один префикс"
                % (from_layer, to_layer),
                "укажите разные слои (feature vs base)")
        want = set(symbols) if symbols else None
        cov_tests = {}   # test_id -> {symbols, pct}
        blk_tests = {}   # test_id -> [blocks]
        traces = {}      # caller -> body dict
        skipped = []     # битые/несогласованные записи (не мигрируются)
        with self.env.begin(readonly=True) as txn:
            cov_dbi = self._opt_dbi(txn, "coverage")
            if cov_dbi is not None:
                scan_pre = ("coverage:" + pre_from).encode()
                with txn.cursor(cov_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre)
                    while rc == mdbx.RC_SUCCESS:
                        if not k.startswith(scan_pre):
                            break
                        test_id = k.decode()[len("coverage:") + len(pre_from):]
                        # битая запись источника не должна ронять миграцию —
                        # пропуск с отчётом (как probe_batch, C-8)
                        try:
                            body = json.loads(v.decode())
                            syms = body.get("symbols", []) or []
                            if want is not None:
                                syms = [s for s in syms if s in want]
                            if want is None or syms:
                                cov_tests[test_id] = {
                                    "symbols": syms,
                                    "pct": float(body.get("pct", 0.0))}
                        except (ValueError, TypeError, AttributeError):
                            skipped.append({"reason": "corrupt-coverage",
                                            "test": test_id})
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            cbk_dbi = self._opt_dbi(txn, "coverage_blocks")
            if cbk_dbi is not None:
                scan_pre = ("coverage_blocks:" + pre_from).encode()
                with txn.cursor(cbk_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre)
                    while rc == mdbx.RC_SUCCESS:
                        if not k.startswith(scan_pre):
                            break
                        # "coverage_blocks:{pre}{test}:{sym}" — ОБА сегмента
                        # могут содержать ":" (test_id свободен, sym = fn:…),
                        # разбор неоднозначен. sym берём из тела (записан в
                        # каждом блоке, _blocks_by_sym), test_id — хвост
                        # ключа без известного суффикса ":"+sym. Для legacy-
                        # записей без symbol в теле — rsplit-fallback; при
                        # НЕсовпадении sym из тела с суффиксом ключа запись
                        # считается несогласованной и пропускается с
                        # отчётом (никакой склейки мусорных ключей).
                        tail = k.decode()[len("coverage_blocks:")
                                          + len(pre_from):]
                        try:
                            blocks = json.loads(v.decode())
                            # тело — список dict-блоков; мусор (не-list или
                            # не-dict элементы) ловим и пропускаем
                            if not isinstance(blocks, list) or \
                                    any(not isinstance(b, dict)
                                        for b in blocks):
                                raise ValueError("non-dict blocks")
                            sym = (blocks[0].get("symbol", "")
                                   if blocks else "")
                        except (ValueError, TypeError, AttributeError):
                            skipped.append({"reason": "corrupt-blocks",
                                            "key": tail})
                            rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                            continue
                        if sym and tail.endswith(":" + sym):
                            test_id = tail[:-(len(sym) + 1)]
                        elif not sym:
                            test_id, sym = tail.rsplit(":", 1)
                        else:
                            # sym в теле ≠ sym в ключе — повреждённая запись:
                            # разбор неоднозначен, мигрировать небезопасно
                            skipped.append({"reason": "sym-mismatch",
                                            "key": tail})
                            rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                            continue
                        if want is not None and sym not in want:
                            rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                            continue
                        blk_tests.setdefault(test_id, []).extend(blocks)
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            ct_dbi = self._opt_dbi(txn, "call_traces")
            if ct_dbi is not None:
                scan_pre = pre_from.encode()
                with txn.cursor(ct_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre)
                    while rc == mdbx.RC_SUCCESS:
                        if not k.startswith(scan_pre):
                            break
                        caller = k.decode()[len(pre_from):]
                        if want is None or caller in want:
                            try:
                                traces[caller] = json.loads(v.decode())
                            except ValueError:
                                skipped.append({"reason": "corrupt-trace",
                                                "caller": caller})
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        n_blocks = sum(len(bl) for bl in blk_tests.values())
        with self._begin_write() as txn:
            for test_id, info in cov_tests.items():
                self._coverage_put_locked(txn, pre_to, test_id,
                                          info["symbols"], info["pct"])
            for test_id, blocks in blk_tests.items():
                self._coverage_blocks_put_locked(txn, pre_to, test_id,
                                                 blocks)
            ct_dbi = self.dbi(txn, "call_traces")
            ctr_dbi = self.dbi(txn, "call_traces_rev")
            for caller, body in traces.items():
                ckey = (pre_to + caller).encode()
                # перезапись caller: обратные рёбра на callee, исчезнувшие
                # из нового тела, должны быть удалены — иначе rev-индекс
                # расходится с прямым (висячие callee→caller пары).
                rc, old_val = txn.get(ct_dbi, ckey)
                if old_val:
                    try:
                        stale = set(json.loads(old_val.decode()).keys()) \
                            - set(body.keys())
                    except ValueError:
                        stale = set()
                    for sc in stale:
                        txn.delete(ctr_dbi, (pre_to + sc).encode(), ckey)
                txn.put(ct_dbi, ckey,
                        json.dumps(body, ensure_ascii=False).encode())
                with txn.cursor(ctr_dbi) as cur:
                    for callee in body.keys():
                        cur.put_nodupe((pre_to + callee).encode(), ckey)
        result = {"from": from_layer, "to": to_layer,
                  "tests": len(cov_tests), "blocks": n_blocks,
                  "traces": len(traces)}
        if skipped:
            result["skipped"] = skipped
        return result

    def coverage_symbols(self, symbol_prefix: str, limit: int = 20,
                         layer: str = "") -> list:
        """Тесты, покрывающие символы с префиксом (covered_by, в слое)."""
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "covered_by")
            if dbi is None:
                return []  # старая БД без covered_by
            scan_pre = "covered_by:" + pre + symbol_prefix
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(scan_pre):
                        break
                    sym = key[len("covered_by:" + pre):]
                    out.append({"symbol": sym, "tests": json.loads(v)})
                    if limit and len(out) >= limit:
                        break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def coverage_remove(self, test_id: str, layer: str = "") -> dict:
        """Удаляет покрытие теста: coverage + covered_by + coverage_blocks.

        Блочная детализация чистится префиксным сканом
        `coverage_blocks:{pre}{test}:*` — иначе блоки удалённого теста
        остаются «живыми» (C-3).
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            cov_dbi = self.dbi(txn, "coverage")
            cb_dbi = self.dbi(txn, "covered_by")
            cbk_dbi = self.dbi(txn, "coverage_blocks")
            key = ("coverage:" + pre + test_id).encode()
            rc, val = txn.get(cov_dbi, key)
            if val is None:
                return {"result": "notfound"}
            symbols = json.loads(val.decode()).get("symbols", [])
            txn.delete(cov_dbi, key)
            for sym in symbols:
                cb_key = ("covered_by:" + pre + sym).encode()
                rc2, v2 = txn.get(cb_dbi, cb_key)
                if v2 is None:
                    continue
                cur = json.loads(v2.decode())
                if test_id in cur:
                    cur.remove(test_id)
                    if cur:
                        txn.put(cb_dbi, cb_key,
                                json.dumps(cur, ensure_ascii=False).encode())
                    else:
                        txn.delete(cb_dbi, cb_key)
            # все блочные записи теста (префиксный скан; после delete курсор
            # уже на следующем элементе — NEXT вместо повторного SET_RANGE)
            blk_pre = ("coverage_blocks:%s%s:" % (pre, test_id)).encode()
            with txn.cursor(cbk_dbi) as cur:
                rc, k, _ = cur.get(mdbx.CURSOR_SET_RANGE, blk_pre)
                while rc == mdbx.RC_SUCCESS:
                    if not k.startswith(blk_pre):
                        break
                    cur.delete()
                    rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
            return {"result": "removed"}

    @staticmethod
    def _blocks_by_sym(blocks: list) -> dict:
        """Агрегация блоков по символу (общая для insert/batch)."""
        by_sym = {}
        for b in blocks:
            sym_bare = b.get("symbol", "")
            by_sym.setdefault(sym_bare, []).append({
                "symbol": sym_bare,  # в теле: summary читает symbol отсюда
                "block_id": b.get("block_id", ""),
                "l0": b.get("l0", 0), "l1": b.get("l1", 0),
                "covered": bool(b.get("covered")),
            })
        return by_sym

    def _coverage_blocks_put_locked(self, txn, pre: str, test_id: str,
                                    blocks: list) -> int:
        """Запись блоков одного теста внутри write-txn (общая точка)."""
        dbi = self.dbi(txn, "coverage_blocks")
        by_sym = self._blocks_by_sym(blocks)
        for sym_bare, bl in sorted(by_sym.items()):
            key = ("coverage_blocks:%s:%s"
                   % (pre + test_id, sym_bare)).encode()
            txn.put(dbi, key, json.dumps(bl, ensure_ascii=False).encode())
        return len(by_sym)

    def coverage_blocks_insert(self, test_id: str, blocks: list,
                               layer: str = "") -> int:
        """Сохраняет блочную детализацию покрытия теста.

        Формат записи: `coverage_blocks:{layer}/test:{sym-bare}` → JSON
        `[{"block_id", "l0", "l1", "covered"}]`. Агрегирует блоки по символу.
        Пустой layer — активная ветка (чтобы feature-прогоны не писали в base).

        N15: семантика ЗАМЕНЫ по символу — повторный вызов для (test, symbol)
        перезаписывает весь список блоков этого символа, а не объединяет.
        Поэтому частичный повторный insert одним символом оставит только
        переданные блоки. Коллекторы пишут полный набор блоков символа за раз
        (run_coverage/gcov_to_coverage), поэтому в штатном пайплайне это
        безопасно; для инкремента читайте текущие блоки и передавайте union.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self._begin_write() as txn:
            return self._coverage_blocks_put_locked(txn, pre, test_id, blocks)

    def coverage_blocks_batch(self, tests: dict, layer: str = "") -> dict:
        """Батчевая запись блоков: {test_id: [blocks]} в ОДНОЙ write-txn (A3).

        Та же семантика, что coverage_blocks_insert, но одна txn на весь
        прогон: run_coverage пишет ~2 txn вместо 2×N (N = число тестов),
        не удерживая writer-лок роя между тестами.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        n_tests, n_syms = 0, 0
        with self._begin_write() as txn:
            for test_id, blocks in (tests or {}).items():
                n_syms += self._coverage_blocks_put_locked(
                    txn, pre, test_id, blocks or [])
                n_tests += 1
        return {"tests": n_tests, "symbols": n_syms}

    def coverage_blocks_get(self, test_id: str, symbol: str,
                            layer: str = "") -> list:
        """Блоки символа, покрытые тестом (или [] если нет данных).

        Ключ согласован с coverage_blocks_insert: `{layer}/test:{sym-bare}`.
        """
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        key = ("coverage_blocks:%s:%s" % (pre + test_id, symbol)).encode()
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "coverage_blocks")
            if dbi is None:
                return []
            rc, val = txn.get(dbi, key)
            if val is None:
                return []
            return json.loads(val.decode())

    def coverage_gaps(self, module: str = "", layer: str = "",
                      limit: int = 100) -> dict:
        """Белые пятна покрытия: символы modules без покрытия (или блоки).

        Агрегирует по символам:
          - uncovered_symbols — символы module, без записи в coverage;
          - uncovered_blocks — блоки, отмеченные covered=false в
            coverage_blocks по всем тестам.
        """
        covd_syms = set()
        uncovered_symbols = []
        # ОДНА read-txn на весь метод (B1): coverage и symbols читаются из
        # одного снимка — раньше symbols шли отдельными txn (OVERLAPPING)
        # и могли быть из другого поколения, чем coverage.
        with self.env.begin(readonly=True) as txn:
            layer = self._effective_layer_locked(txn, layer)
            pre = self.layer_prefix(layer)
            cov_dbi = self.dbi(txn, "coverage")
            with txn.cursor(cov_dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    # тест-id в ключе: "coverage:{layer}/test" для feature-слоя
                    test_part = key[len("coverage:"):] if key.startswith("coverage:") else key
                    if (pre and test_part.startswith(pre)) or \
                       (not pre and not _LAYER_KEY_RE.match(test_part)):
                        body = json.loads(v)
                        for s in body.get("symbols", []) or []:
                            covd_syms.add(s)
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            all_syms = self._map_symbols_locked(txn, pre, "fn:")
            mod_pat = module
            for sym in all_syms:
                body = self._map_symbol_locked(txn, pre, sym)
                # merged-карты: module внутри implementations
                mods = [i.get("module")
                        for i in body.get("implementations") or []
                        if i.get("module")]
                if body.get("module"):
                    mods.insert(0, body.get("module", ""))
                if mod_pat and not any(
                        m and m.startswith(mod_pat) for m in mods):
                    continue
                if sym not in covd_syms:
                    uncovered_symbols.append({
                        "symbol": sym,
                        "module": mods[0] if mods else body.get("module", ""),
                    })
                    if limit and len(uncovered_symbols) >= limit:
                        break
        return {
            "module": module, "layer": layer,
            "uncovered_symbols": uncovered_symbols,
            "uncovered_symbols_count": len(uncovered_symbols),
        }

    def coverage_get(self, test_id: str, layer: str = "") -> dict:
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "coverage")
            if dbi is None:
                return {}
            rc, val = txn.get(dbi, ("coverage:" + pre + test_id).encode())
            if val is None:
                return {}
            return json.loads(val.decode())

    def coverage_all(self, limit: int = None, layer: str = "") -> list:
        # F5.2/R2: пустой список на старой схеме — ошибка, а не «нет покрытия»
        self._check_opt_table("coverage")
        layer = self._effective_layer(layer)
        pre = self.layer_prefix(layer)
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "coverage")
            if dbi is None:
                return []
            scan_pre = "coverage:" + pre
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre.encode())
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if not key.startswith(scan_pre):
                        break
                    # базовый слой не должен видеть чужие feature-тесты
                    test_part = key[len("coverage:"):]
                    if not pre and _LAYER_KEY_RE.match(test_part):
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                        continue
                    out.append({"test": test_part, "body": json.loads(v)})
                    if limit and len(out) >= limit:
                        break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def coverage_summary(self, module: str = "", layer: str = "") -> dict:
        """Сводка покрытия слоя: символы и блоки, покрытые ≥1 тестом (union).

        По модулям + итого: symbols_covered/total, blocks_covered/total, pct.
        Примитив дашборда «белые пятна» для итеративного развития покрытия.
        """
        covd_syms = set()
        blocks_agg = {}  # sym -> [set(total block_ids), set(covered block_ids)]
        mods = {}
        # ОДНА read-txn на весь метод (B1): coverage, coverage_blocks
        # и symbols — из одного снимка; раньше итерация символов шла
        # отдельными txn (OVERLAPPING) и могла расходиться с coverage.
        with self.env.begin(readonly=True) as txn:
            layer = self._effective_layer_locked(txn, layer)
            pre = self.layer_prefix(layer)
            cov_dbi = self._opt_dbi(txn, "coverage")
            cbk_dbi = self._opt_dbi(txn, "coverage_blocks")
            if cov_dbi is not None:
                scan_pre = "coverage:" + pre
                with txn.cursor(cov_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, scan_pre.encode())
                    while rc == mdbx.RC_SUCCESS:
                        key = k.decode()
                        if not key.startswith(scan_pre):
                            break
                        test_part = key[len("coverage:"):]
                        if not pre and _LAYER_KEY_RE.match(test_part):
                            rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                            continue
                        for sym in json.loads(v).get("symbols", []) or []:
                            covd_syms.add(sym)
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            if cbk_dbi is not None:
                blk_pre = "coverage_blocks:" + pre
                with txn.cursor(cbk_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, blk_pre.encode())
                    while rc == mdbx.RC_SUCCESS:
                        key = k.decode()
                        if not key.startswith(blk_pre):
                            break
                        # fallback для старых записей: symbol из ключа
                        # ("coverage_blocks:{pre}{test}:{sym}" — split c limit=2,
                        # т.к. sym сам содержит ":")
                        key_sym = key.split(":", 2)[2] if key.count(":") >= 2 else ""
                        for b in json.loads(v):
                            sym = b.get("symbol") or key_sym
                            if not sym:
                                continue
                            bid = b.get("block_id", "")
                            # union по block_id: [total_ids, covered_ids].
                            # Set'ы НУЖНЫ для точности union (один block_id
                            # встречается в разных тестах); выигрыш B2 здесь
                            # не в памяти, а в однопроходности и одной txn.
                            agg = blocks_agg.setdefault(sym, [set(), set()])
                            agg[0].add(bid)
                            if b.get("covered"):
                                agg[1].add(bid)
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
            # символы слоя → по модулям (в той же txn)
            for sym in self._map_symbols_locked(txn, pre, ""):
                body = self._map_symbol_locked(txn, pre, sym)
                mod_list = [i.get("module")
                            for i in body.get("implementations") or []
                            if i.get("module")]
                if body.get("module"):
                    mod_list.insert(0, body.get("module", ""))
                mod = mod_list[0] if mod_list else body.get("module", "?")
                if module and not mod.startswith(module):
                    continue
                m = mods.setdefault(mod, {"symbols_total": 0,
                                          "symbols_covered": 0,
                                          "blocks_total": 0,
                                          "blocks_covered": 0})
                m["symbols_total"] += 1
                if sym in covd_syms:
                    m["symbols_covered"] += 1
                agg = blocks_agg.get(sym)
                if agg:
                    m["blocks_total"] += len(agg[0])
                    m["blocks_covered"] += len(agg[1])
        tot = {"symbols_total": 0, "symbols_covered": 0,
               "blocks_total": 0, "blocks_covered": 0}
        for m in mods.values():
            for kk in tot:
                tot[kk] += m[kk]
            m["pct"] = round(100.0 * m["blocks_covered"] / m["blocks_total"], 1) \
                if m["blocks_total"] else (
                    round(100.0 * m["symbols_covered"] / m["symbols_total"], 1)
                    if m["symbols_total"] else 0.0)
        tot["pct"] = round(100.0 * tot["blocks_covered"] / tot["blocks_total"], 1) \
            if tot["blocks_total"] else (
                round(100.0 * tot["symbols_covered"] / tot["symbols_total"], 1)
                if tot["symbols_total"] else 0.0)
        return {"layer": layer, "module": module, "modules": mods, "total": tot}

    def ci_jobs_insert(self, job_key: str, body: dict) -> dict:
        """Фиксирует параметры CI-джобы ({workflow}/{job})."""
        with self._begin_write() as txn:
            txn.put(self.dbi(txn, "ci_jobs"), job_key.encode(),
                    json.dumps(body, ensure_ascii=False).encode())
            return {"job": job_key}

    def ci_jobs_get(self, job_key: str) -> dict:
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "ci_jobs")
            if dbi is None:
                return {}
            rc, val = txn.get(dbi, job_key.encode())
            if val is None:
                return {}
            return json.loads(val.decode())

    def ci_jobs_by_test(self, test_id: str) -> list:
        """Джобы, в которых исполняется тест (по labels/шагам в ci_jobs)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "ci_jobs")
            if dbi is None:
                return []
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    body = json.loads(v)
                    tests = body.get("tests") or body.get("tests_hint") or []
                    if test_id in tests:
                        out.append(k.decode("utf-8", "replace"))
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def ci_jobs_all(self, prefix: str = "", limit: int = None) -> list:
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "ci_jobs")
            if dbi is None:
                return []  # старая БД без ci_jobs
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode("utf-8", "replace")
                    if key.startswith(prefix):
                        out.append({"job": key, "body": json.loads(v)})
                        if limit and len(out) >= limit:
                            break
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def stats(self) -> dict:
        by_type = {}
        total_importance = 0.0
        n = 0
        incoming_top = []
        # ОДНА read-txn на весь метод (B1): records + счётчики входящих +
        # next_id из одного снимка — раньше на каждую запись открывалась
        # своя txn (N+1) плюс отдельная на next_id.
        next_id = self.next_id
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            try:
                rc, nid_val = txn.get(self.dbi(txn, "meta"), b"next_id")
                if nid_val is not None and len(nid_val) == ID_LEN:
                    next_id = _unpack_u64(nid_val)
            except Exception:  # noqa: BLE001
                pass
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    body = json.loads(val.decode())
                    t = body.get("type", "?")
                    by_type[t] = by_type.get(t, 0) + 1
                    total_importance += body.get("importance", 0.0)
                    n += 1
                    rec_id = body.get("_id", 0)
                    inc = self._incoming_count(txn, rec_id)
                    incoming_top.append((key.decode(), inc))
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        incoming_top.sort(key=lambda kv: -kv[1])
        return {
            "records": n,
            "by_type": by_type,
            "avg_importance": round(total_importance / n, 3) if n else 0.0,
            "top_linked": incoming_top[:10],
            "vocab_modules": len(self.norm.modules),
            "vocab_topics": len(self.norm.topics),
            "last_gc": self.last_gc,
            "next_id": next_id,
        }

    def _peek_next_id(self) -> int:
        """Текущее значение meta.next_id в read-txn (для stats/логов)."""
        try:
            with self.env.begin(readonly=True) as txn:
                rc, val = txn.get(self.dbi(txn, "meta"), b"next_id")
                return _unpack_u64(val) if val is not None and len(val) == ID_LEN else 1
        except Exception:  # noqa: BLE001
            return self.next_id

    def version_info(self) -> dict:
        """Полная версионная картина для оперативного контроля.

        Возвращает: версию модуля, версию libmdbx (из экспортируемого
        mdbx_version), версию схемы данных (persisted в meta.schema_ver) и
        версию данных (поколение карты canary.y + число записей + символов).
        """
        try:
            lib_ver = mdbx.version_string()
        except Exception:  # noqa: BLE001
            lib_ver = "?"
        st = self.stats()
        try:
            gen = self.map_generations()
            data_gen = gen["generation"]
            data_symbols = gen["symbols"]
        except Exception:  # noqa: BLE001
            data_gen, data_symbols = None, None
        schema_ver = SCHEMA_VERSION
        try:
            with self.env.begin(readonly=True) as txn:
                rc, vmeta = txn.get(self.dbi(txn, "meta"), b"schema_ver")
                if vmeta is not None:
                    schema_ver = vmeta.decode()
        except Exception:  # noqa: BLE001
            pass
        try:
            disk_int = int(schema_ver)
        except ValueError:
            disk_int = None
        # F5.2/R2: семантика for агента — bool matches + ДИРЕКЦИЯ:
        # too_new (БД новее модуля — обновить MCP) vs outdated (БД старше
        # модуля — надо переключиться в rw для миграции), vs matches (норма)
        schema_matches = schema_ver == SCHEMA_VERSION
        schema_too_new = (disk_int is not None
                          and disk_int > int(SCHEMA_VERSION))
        schema_outdated = (schema_ver != SCHEMA_VERSION and not schema_too_new)
        return {
            "module_version": MODULE_VERSION,
            "libmdbx_version": lib_ver,
            "schema_version": schema_ver,
            "schema_matches": schema_matches,
            "schema_too_new": schema_too_new,
            "schema_outdated": schema_outdated,
            "branch": self.branch_current(),
            "data": {
                "map_generation": data_gen,
                "records": st.get("records"),
                "symbols": data_symbols,
                "next_id": st.get("next_id"),
            },
        }

    # --- слои веток: нелинейное версионирование БЗ ---------------------------
    @staticmethod
    def layer_prefix(layer: str = "") -> str:
        """Префикс слоя для перегенерируемых таблиц.

        Базовая ветка ("" или "0.15.base" и т.п.) живёт БЕЗ префикса
        (обратная совместимость); любой другой слой — с префиксом
        `{layer}/`. Стабильность: версия (0.15) важнее имени ветки
        (master→stable→lts) — см. docs/BRANCHES.md.
        """
        layer = (layer or "").strip().strip("/")
        if not layer:
            return ""
        # "0.15.base"/"0.15" — базовые слои: без префикса
        if layer.endswith(".base") or _VVER_RE.fullmatch(layer):
            return ""
        return layer + "/"

    def _effective_layer(self, layer: str = "") -> str:
        """Слой для чтения/записи: явный аргумент > env процесса > активная ветка.

        Кайдзен (окт 2026): `branch_activate` должен влиять на map/coverage/
        probe-инструменты по умолчанию — иначе активация декларативна, а
        данные читаются из базового слоя (путаница в многобраншевой работе).
        Env-уровень (SHARED_GRAPH_MEMORY_LAYER) изолирует процесс роя от
        чужих branch_activate (N1).
        """
        if layer:
            return layer
        if self._env_layer:
            return self._env_layer
        try:
            return self.branch_current().get("layer", "")
        except Exception:  # noqa: BLE001
            return ""

    def _effective_layer_locked(self, txn, layer: str = "") -> str:
        """_effective_layer, читающий branch_current ВНУТРИ переданной txn.

        Используется там, где снимок должен быть атомарным (map_snapshot):
        иначе разрешение слоя открыло бы вторую read-txn (C-1).
        Env-уровень применяется до meta — в той же иерархии (N1).
        """
        if layer:
            return layer
        if self._env_layer:
            return self._env_layer
        try:
            rc, val = txn.get(self.dbi(txn, "meta"), b"branch_current")
            layer = val.decode() if val else ""
            if not layer:
                rc2, rcval = txn.get(self.dbi(txn, "meta"), b"branch_ver")
                layer = rcval.decode() if rcval else ""
            return layer
        except Exception:  # noqa: BLE001
            return ""

    def branch_register(self, ver_slug: str, feature_slug: str = "base",
                        n_commits: int = 0, revision: str = "",
                        git_tag: str = "", base_branch: str = "",
                        status: str = "active") -> dict:
        """Регистрирует слой ветки (нелинейное версионирование).

        Идентификатор слоя: `{ver-slug}.{feature-slug}.{n}` (см. BRANCHES.md).
        Базовая ветка (`feature`="base"|"") живёт без префикса.
        """
        ver = ver_slug.strip().lstrip("v")
        feat = (feature_slug or "base").strip().strip("/")
        if not ver:
            raise invalid("branch: не указан версионный slug",
                          "укажите ver_slug (например 0.15)")
        feat_clean = re.sub(r"[^A-Za-z0-9._-]", "-", feat)
        layer = "%s.%s.%d" % (ver, feat_clean, int(n_commits)) if feat_clean != "base" else ver
        body = {
            "ver_slug": ver,
            "feature_slug": feat_clean,
            "n_commits": int(n_commits),
            "revision": revision,
            "git_tag": git_tag,
            "base_branch": base_branch,
            "status": status,
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "is_base": feat_clean == "base",
        }
        with self._begin_write() as txn:
            txn.put(self.dbi(txn, "branches"), ("branch:" + layer).encode(),
                    json.dumps(body, ensure_ascii=False).encode())
        return {"layer": layer, "body": body}

    def branch_list(self, base_only: bool = False) -> list:
        # F5.2/R2: пустой список на старой схеме молча неотличим от «нет
        # веток» (ревью #164-r5): отсутствие таблицы на старой схеме — ошибка
        self._check_opt_table("branches")
        out = []
        with self.env.begin(readonly=True) as txn:
            dbi = self._opt_dbi(txn, "branches")
            if dbi is None:
                return []  # актуальная схема + read-only старт: таблицы нет
            with txn.cursor(dbi) as cur:
                rc, k, v = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    body = json.loads(v)
                    if not base_only or body.get("is_base"):
                        out.append({"layer": k.decode()[len("branch:"):],
                                    "body": body})
                    rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        return out

    def branch_current(self) -> dict:
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "meta"), b"branch_current")
            layer = val.decode() if val else ""
            if not layer:
                rc2, rcval = txn.get(self.dbi(txn, "meta"), b"branch_ver")
                layer = rcval.decode() if rcval else ""
            return {"layer": layer}

    def branch_activate(self, layer: str, expected_prev=None) -> dict:
        """Переключает активный слой (глобальный default чтения/записи).

        `expected_prev` — CAS-защита от гонки в рое (N1): если задан и не
        совпал с текущим активным слоем, активация отвергается ошибкой
        conflict — параллельный агент уже переключил ветку. Без аргумента
        поведение прежнее (безусловная установка).
        """
        layer = layer.strip().strip("/")
        with self._begin_write() as txn:
            meta = self.dbi(txn, "meta")
            rc, val = txn.get(meta, b"branch_current")
            prev = val.decode() if val else ""
            if expected_prev is not None and prev != expected_prev:
                raise MemoryError(
                    "CONFLICT", "conflict",
                    "активный слой %r не совпал с ожидаемым %r — "
                    "переключён другим агентом" % (prev, expected_prev),
                    "перечитайте branch_current и повторите с актуальным "
                    "expected_prev", "retry once")
            txn.put(meta, b"branch_current", layer.encode())
            # canary.z = активный слой (для внешних читателей/отладки)
            c = txn.canary_get()
            txn.canary_put(x=c.get("x", 0), y=c.get("y", 0), z=len(layer))
            # N14: активация НЕзарегистрированного слоя — операторская
            # ловушка (чтения молча вернут пустой слой). Не ошибка (слой
            # мог быть очищен purge'ем намеренно), но предупреждаем явно.
            registered = False
            if layer:
                br_dbi = self._opt_dbi(txn, "branches")
                if br_dbi is not None:
                    rc2, rv = txn.get(br_dbi, ("branch:" + layer).encode())
                    registered = rv is not None
            out = {"previous": prev, "current": layer}
            if not registered:
                out["warning"] = (
                    "слой %r отсутствует в реестре branches — чтения/записи "
                    "пойдут в пустой слой; зарегистрируйте его "
                    "(branch_register) или исправьте опечатку" % layer)
            return out

    def branch_remove(self, layer: str) -> dict:
        layer = layer.strip().strip("/")
        with self._begin_write() as txn:
            dbi = self.dbi(txn, "branches")
            key = ("branch:" + layer).encode()
            rc, val = txn.get(dbi, key)
            if val is None:
                return {"result": "notfound"}
            txn.delete(dbi, key)
            return {"result": "removed", "layer": layer}

    def branch_purge(self, layer: str) -> dict:
        """Удаляет слой ветки С ДАННЫМИ во всех перегенерируемых таблицах.

        Жизненный цикл слоя (N5): после rebase/переписывания истории старый
        slug {ver}.{feat}.{n} мёртв — branch_remove оставляет его данные
        мусором навсегда (таблицы растут монотонно). Purge чистит все ключи
        `{layer}/...` в symbols/call_edges(_rev)/groups/call_traces(_rev)/
        coverage/covered_by/coverage_blocks/probes, пары id↔key слоя в
        sym_ids/sym_id2key + строку реестра branches.

        Базовый слой (pre=="") purge ЗАПРЕЩЁН: он — корень карты/покрытия;
        его обновление = replace-загрузка (map_load_batch/coverage_batch),
        не удаление. АКТИВНЫЙ слой (branch_current) purge тоже ЗАПРЕЩЁН:
        иначе meta.branch_current укажет на мёртвый слой и все чтения с
        layer="" будут молча пусты — сначала переключите ветку.
        Возвращает счётчики удалённых ключей по таблицам.
        """
        layer = layer.strip().strip("/")
        pre = self.layer_prefix(layer)
        if not pre:
            raise invalid(
                "branch_purge: базовый слой %r удалять нельзя" % layer,
                "purge предназначен для feature-слоёв; базовый слой "
                "обновляется replace-загрузкой карты")
        if self.layer_prefix(self.branch_current().get("layer", "")) == pre:
            raise invalid(
                "branch_purge: слой %r является активным" % layer,
                "сначала переключите активный слой "
                "(branch_activate на другой или базовый)")
        # схема префиксов ключей по таблицам (см. комментарии _TABLES):
        # map-family — "{pre}", coverage-family/probes — "family:{pre}"
        prefixes = {
            "symbols": pre, "call_edges": pre, "call_edges_rev": pre,
            "groups": pre, "call_traces": pre, "call_traces_rev": pre,
            "coverage": "coverage:" + pre,
            "covered_by": "covered_by:" + pre,
            "coverage_blocks": "coverage_blocks:" + pre,
            "probes": "probe:" + pre,
        }
        removed = {}
        had_reg = False
        with self._begin_write() as txn:
            for table, scan_pre in prefixes.items():
                dbi = self._opt_dbi(txn, table)
                if dbi is None:
                    continue  # старая БД без таблицы
                n = 0
                with txn.cursor(dbi) as cur:
                    rc, k, _ = cur.get(mdbx.CURSOR_SET_RANGE,
                                       scan_pre.encode())
                    while rc == mdbx.RC_SUCCESS:
                        if not k.startswith(scan_pre.encode()):
                            break
                        cur.delete()  # MDBX_CURRENT: текущая k/v-пара
                        n += 1        # (DUPSORT: по дупу за проход)
                        rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
                if n:
                    removed[table] = n
            # sym_ids/sym_id2key: числовые id символов слоя (ключи "{pre}…").
            # Таблицы глобальные, но skey несёт префикс слоя — удаляем пары
            # id↔key слоя, иначе после purge остаются сироты (P3: монотонный
            # рост таблиц при циклах rebase→purge — та самая деградация,
            # которую purge призван лечить).
            sid_dbi = self._opt_dbi(txn, "sym_ids")
            id2key_dbi = self._opt_dbi(txn, "sym_id2key")
            if sid_dbi is not None:
                n = 0
                with txn.cursor(sid_dbi) as cur:
                    rc, k, v = cur.get(mdbx.CURSOR_SET_RANGE, pre.encode())
                    while rc == mdbx.RC_SUCCESS:
                        if not k.startswith(pre.encode()):
                            break
                        if id2key_dbi is not None and v is not None \
                                and len(v) == ID_LEN:
                            txn.delete(id2key_dbi, v)
                        cur.delete()
                        n += 1
                        rc, k, v = cur.get(mdbx.CURSOR_NEXT)
                if n:
                    removed["sym_ids"] = n
            # строка реестра (если есть)
            br_dbi = self._opt_dbi(txn, "branches")
            if br_dbi is not None:
                rc, val = txn.get(br_dbi, ("branch:" + layer).encode())
                had_reg = val is not None
                if had_reg:
                    txn.delete(br_dbi, ("branch:" + layer).encode())
        if not removed and not had_reg:
            return {"result": "notfound", "layer": layer}
        return {"result": "purged", "layer": layer, "removed": removed}

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        # сбросить накопленные LRU-касания и остановить фоновый тред синка
        try:
            self._flush_touches()
        except Exception as e:
            sys.stderr.write("flush touches: %s\n" % e)
        self._stop_sync_thread()
        try:
            self.env.sync(force=True, nonblock=False)
        except Exception as e:
            sys.stderr.write("env sync: %s\n" % e)
        self.env.close()

    def __enter__(self) -> "Store":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# --- утилиты упаковки access ----------------------------------------------------
def struct_double(b: bytes) -> float:
    import struct as _struct
    return _struct.unpack("<d", b)[0]


def pack_access(score: float, ts: int, count: int) -> bytes:
    import struct as _struct
    return (_struct.pack("<d", score)
            + _pack_u64(ts) + _pack_u64(count))


def unpack_access(b: bytes):
    import struct as _struct
    assert len(b) == ACCESS_LEN, "bad access length %d" % len(b)
    score = _struct.unpack("<d", b[0:8])[0]
    ts = _unpack_u64(b[8:16])
    cnt = _unpack_u64(b[16:24])
    return score, ts, cnt
