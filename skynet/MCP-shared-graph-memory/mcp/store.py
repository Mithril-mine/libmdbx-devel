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

import json
import math
import os
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
    internal,
    invalid,
    parse,
    readonly_mode,
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
    }

    def __init__(self, path: str, maxdbs: int = 32, max_value_bytes: int = 16 << 20,
                 sync_mode: str = "safe_nosync", readonly: bool = False,
                 _skip_sync_thread: bool = False):
        self.path = path
        self.max_value_bytes = max_value_bytes
        self.sync_mode = sync_mode
        self._readonly = readonly
        # read-only открытие НЕ должно трогать LCK на запись: используем
        # ACCEDE чтобы присоединиться к уже открытой БД без конфликта флагов.
        self.env = mdbx.Env(path, maxdbs=maxdbs, readonly=readonly,
                            accede=readonly, sync_mode=sync_mode)
        self.norm = Normalizer()
        self._dbi = {}
        # открываем все таблицы один раз (CREATE) в первой write-txn;
        # в read-only режиме CREATE не нужен — dbi открываются с DEFAULTS
        txn_flags = mdbx.TXN_RDONLY if readonly else mdbx.TXN_READWRITE
        with self.env.begin(readonly=readonly) as txn:
            for name in self.TABLES:
                fl = self.TABLES[name] | (mdbx.CREATE if not readonly else 0)
                self._dbi[name] = txn.open_dbi(name, fl)
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
        self._protect_lifeline()
        # Фоновый тред: раз в секунду mdbx_env_sync_poll (дешёвый nonblocking
        # сброс по порогам sync_bytes/sync_period) + контроль st_nlink.
        # Транзакций не создаёт. В read-only режиме тред не запускаем
        # (синк не нужен и может писать в LCK).
        self._sync_stop = threading.Event()
        self._sync_thread = None
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
        except OSError:
            pass
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
                "db_backup kind=user (сделать) или db_backup skip=true (пропустить)")
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
            "gc_prof_available": bool(self._latencies and
                                      self._latencies[-1].get("gc_prof")),
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

    def _save_backup_state(self, st: dict) -> None:
        try:
            os.makedirs(self.backup_dir, exist_ok=True)
            tmp = self._state_path() + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(st, fh)
            os.replace(tmp, self._state_path())
        except OSError:
            pass

    def _list_backups(self, prefix: str = "") -> list:
        out = []
        try:
            for fn in sorted(os.listdir(self.backup_dir)):
                if (fn.startswith(prefix) and fn.endswith(".mdbx")
                        and fn != BACKUP_LIFELINE):
                    p = os.path.join(self.backup_dir, fn)
                    out.append({"name": fn, "path": p,
                                "size": os.path.getsize(p)})
        except OSError:
            pass
        return out

    def _backup_dir_size(self) -> int:
        """Суммарный размер независимых копий (без жёстких ссылок)."""
        total = 0
        try:
            for fn in os.listdir(self.backup_dir):
                if fn.endswith(".mdbx") and fn != BACKUP_LIFELINE:
                    total += os.path.getsize(os.path.join(self.backup_dir, fn))
        except OSError:
            pass
        return total

    def _rotation_candidates(self, keep: int = None) -> list:
        """Старейшие AUTO-копии сверх лимита — кандидаты на удаление."""
        keep = keep or self.backup_keep
        files = self._list_backups("auto-")
        if len(files) <= keep:
            return []
        return files[:-keep]  # sorted по имени = по времени

    def backup(self, kind: str = "user", dest: str = None) -> dict:
        """Консистентная копия БД (MVCC-снапшот) через txn_copy2pathname.

        kind: 'auto' — тихий интервал; 'user' — с запросом/явно; 'manual' —
        ручной db_backup. Имя: {kind}-YYYYMMDD-HHMMSS_txnid<N>.mdbx.
        Флаги: COMPACT | OVERWRITE (компактификация).
        """
        if self.db_file_deleted:
            self._raise_if_deleted()
        if kind not in ("auto", "user", "manual"):
            raise invalid("неизвестный kind бэкапа %r" % kind,
                          "используйте auto|user|manual")
        os.makedirs(self.backup_dir, exist_ok=True)
        with self.env.begin(readonly=True) as txn:
            txnid = txn.id()
            if dest is None:
                name = "%s-%s_txnid%d.mdbx" % (
                    kind, time.strftime("%Y%m%d-%H%M%S"), txnid)
                dest = os.path.join(self.backup_dir, name)
            r = txn.copy2pathname(dest, mdbx.CP_BACKUP)
        return {"path": r["dest"], "txnid": r["txnid"], "kind": kind,
                "size": os.path.getsize(r["dest"])}

    def maybe_run_auto_backup(self) -> None:
        """Таймеры AUTO (тихо) и PROMPT (с запросом); 0 = выключено."""
        if os.name == "nt" or self.db_file_deleted:
            return
        now = time.time()
        st = self._load_backup_state()
        if (self.auto_backup_seconds > 0
                and now - st.get("last_auto", 0) >= self.auto_backup_seconds):
            if self._backup_dir_size() > self.backup_max_bytes:
                self.backup_notice = (
                    "auto-backup пропущен: каталог бэкапов превышает лимит "
                    "%d МБ" % (self.backup_max_bytes >> 20))
            else:
                try:
                    self.backup(kind="auto")
                except Exception as e:  # noqa: BLE001
                    self.backup_notice = "auto-backup failed: %s" % e
            st["last_auto"] = now
            self._save_backup_state(st)
        if (self.prompt_backup_seconds > 0 and not self.backup_pending
                and now - st.get("last_prompt", 0) >= self.prompt_backup_seconds):
            self.backup_pending = True

    def run_prompt_backup(self, approve: bool) -> dict:
        """Ответ агента на PROMPT-запрос: сделать/пропустить копию."""
        if not self.backup_pending:
            return {"pending": False}
        self.backup_pending = False
        st = self._load_backup_state()
        result = {"pending": False, "approved": approve}
        if approve:
            try:
                r = self.backup(kind="user")
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
                pass
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
        now = time.time()
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
            "db_file_deleted": self.db_file_deleted,
            "notice": self.backup_notice,
            "lifeline_error": self._lifeline_error,
            "tiers": {
                "lifeline": os.path.isfile(os.path.join(self.backup_dir,
                                                        BACKUP_LIFELINE)),
                "auto": len(self._list_backups("auto-")),
                "user": len(self._list_backups("user-")),
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
            def __enter__(self): return None
            def __exit__(self, *exc): return False
        return _G()

    def _reopen(self, readonly: bool) -> None:
        """Закрыть и переоткрыть env (синхронно, без потери данных)."""
        self._stop_sync_thread()
        try:
            self.env.sync(force=True, nonblock=False)
        except Exception:
            pass
        self.env.close()
        self._readonly = readonly
        self.env = mdbx.Env(self.path, maxdbs=32, readonly=readonly,
                            accede=readonly, sync_mode=self.sync_mode)
        self._dbi = {}
        with self.env.begin(readonly=readonly) as txn:
            for name in self.TABLES:
                fl = self.TABLES[name] | (mdbx.CREATE if not readonly else 0)
                self._dbi[name] = txn.open_dbi(name, fl)
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
            cur = txn.cursor(self.dbi(txn, "vocab"))
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
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "meta"), b"next_id")
            return _unpack_u64(val) if val is not None else 1

    def dbi(self, txn, name: str) -> int:
        return self._dbi[name]

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
                if attempt < BUSY_RETRIES:
                    time.sleep(0.05 * (attempt + 1))
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
        """Одна write-txn на все накопленные LRU-касания (вместо N мелких)."""
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

    def safe_store(self, raw_key: str, rtype: str, summary: str, importance: float,
                   date: str = ""):
        """Атомарная операция сохранения с дедупликацией. Одна write-txn."""
        if rtype not in RECORD_TYPES:
            raise invalid(
                "тип %r не из допустимого набора" % rtype,
                "используйте один из: %s" % ", ".join(RECORD_TYPES),
            )
        key = self.norm.normalize(raw_key)
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

            record_id = self.next_id
            self.next_id += 1
            txn.put(self.dbi(txn, "meta"), b"next_id", _pack_u64(self.next_id))

            self._put_record(txn, key, self._body(rtype, summary, importance,
                                                  date, record_id, terms))
            self._index_add(txn, key, record_id, terms, summary)
            return {"result": "created", "key": key, "id": record_id}

    def _merge(self, txn, key, rtype, summary, importance, date, terms, old_val):
        old = json.loads(old_val.decode())
        old_terms = old.get("_terms", [])
        record_id = old.get("_id", self.next_id)
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
            old_ts = _unpack_u64(old[8:16])
            old_cnt = _unpack_u64(old[16:24])
        else:
            old_score = score if score is not None else 0.5
            old_ts = _now()
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
                rec_id = _unpack_u64(idk)
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

    def recall(self, pattern: str, limit: int = 5):
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

    def search(self, query: str, limit: int = 5):
        """Поиск по термам с пересечением постинг-списков и ранжированием."""
        limit = max(1, min(int(limit), 10))
        keys = self.lookup(query, limit=1000)
        if not keys:
            return []
        rows = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            for k in keys:
                rc, val = txn.get(rec_dbi, k.encode())
                if val is None:
                    continue
                body = json.loads(val.decode())
                rows.append(self._decorate(txn, k, body))
        rows.sort(key=lambda r: r.get("score", 0.0), reverse=True)
        top = rows[:limit]
        for r in top:
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
            return {"result": "unlinked" if r1 else "notfound"}

    def graph(self, key: str, depth: int = 1):
        depth = max(0, min(int(depth), 3))
        root = self.norm.normalize(key)
        with self.env.begin(readonly=True) as txn:
            rc, _ = txn.get(self.dbi(txn, "records"), root.encode())
        if rc != mdbx.RC_SUCCESS:
            raise invalid(
                "запись %r не существует" % root,
                "сначала сохраните её через safe_store",
            )
        nodes = {root}
        edges = []
        visited = set()
        frontier = [root]
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            id2key_dbi = self.dbi(txn, "id2key")
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
            record_id = self.next_id
            self.next_id += 1
            ctx["_id"] = record_id
            ctx["_terms"] = []
            body = json.dumps(ctx, ensure_ascii=False, sort_keys=True).encode()
            k = key.encode()
            self._check_size(k, body)
            txn.put(self.dbi(txn, "records"), k, body)
            txn.put(self.dbi(txn, "meta"), b"next_id", _pack_u64(self.next_id))
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

    def gc(self, dry_run: bool = True, archive: bool = False, threshold: float = GC_WARM):
        hot, warm, cold = [], [], []
        ids_to_cold = []
        for key, val in self._all_records():
            body = json.loads(val.decode())
            if body.get("type") == "proc" and "context-snapshot" in key.decode():
                hot.append((key, None))
                continue
            rec_id = body.get("_id", 0)
            with self.env.begin(readonly=True) as txn:
                incoming = self._incoming_count(txn, rec_id)
                score = self._score_of(txn, rec_id, body.get("importance", 0.0), incoming)
            item = (key, score)
            if score > GC_HOT:
                hot.append(item)
            elif score > threshold:
                warm.append(item)
            else:
                cold.append(item)
                ids_to_cold.append((key, rec_id, body))
        stats = {
            "hot": len(hot), "warm": len(warm), "cold": len(cold),
            "dry_run": dry_run, "archive": archive,
            "cold_keys": [k.decode() if isinstance(k, bytes) else k for k, _ in cold],
        }
        if not dry_run and archive and cold:
            moved = 0
            with self._begin_write() as txn:
                arch_dbi = self.dbi(txn, "archive")
                for key, rec_id, body in ids_to_cold:
                    txn.put(arch_dbi, key,
                            json.dumps(body, ensure_ascii=False).encode())
                    self._delete_record(txn, key.decode(), body)
                    moved += 1
            stats["archived"] = moved
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

    def _delete_record(self, txn, key: str, body: dict) -> None:
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

    def _delete_links_of(self, txn, links_dbi, idb, rec_id) -> None:
        # удаляем все рёбра, где субъект = rec_id
        prefix = idb + b"\0"
        with txn.cursor(links_dbi) as cur:
            rc, lk, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
            while rc == mdbx.RC_SUCCESS:
                if not lk.startswith(prefix):
                    break
                cur.delete()
                rc, lk, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
        # удаляем рёбра, где объект = rec_id (обратные/прямые)
        with txn.cursor(links_dbi) as cur:
            rc, lk, lv = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                nxt = cur.get(mdbx.CURSOR_NEXT)
                if _unpack_u64(lv) == rec_id:
                    cur.delete()
                rc, lk, lv = nxt

    # --- refactoring-map (структурный слой) ---------------------------------
    def map_symbol(self, key: str) -> dict:
        """Читает символ из structural-слоя (symbols)."""
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "symbols"), key.encode())
        if val is None:
            return {}
        return json.loads(val.decode())

    def sym_id(self, key: str) -> int:
        """Стабильный числовой id символа.

        Числовой id выделяется из ОБЩЕГО счётчика `meta/next_id` (того же,
        что для записей знаний): links/coverage оперируют единым числовым
        пространством, пересечение диапазонов символов и записей было бы
        неоднозначностью. Отображения sym_ids/sym_id2key НЕ чистятся при
        replace — числовые ссылки (links) остаются стабильными между
        регенерациями.
        """
        with self._begin_write() as txn:
            sid_dbi = self.dbi(txn, "sym_ids")
            rc, val = txn.get(sid_dbi, key.encode())
            if val is not None and len(val) == 8:
                return _unpack_u64(val)
            nid = self.next_id
            self.next_id += 1
            txn.put(self.dbi(txn, "meta"), b"next_id",
                    _pack_u64(self.next_id))
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

    def map_symbols(self, prefix: str = "", limit: int = None) -> list:
        """Все ключи symbols по префиксу (детерминированная сортировка)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            sym_dbi = self.dbi(txn, "symbols")
            with txn.cursor(sym_dbi) as cur:
                rc, k, _ = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    key = k.decode()
                    if not prefix or key.startswith(prefix):
                        out.append(key)
                        if limit and len(out) >= limit:
                            break
                    rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
        return out

    def map_put_symbol(self, key: str, body: dict) -> None:
        """Аддитивная запись символа (перегенерируемый слой).

        Присваивает стабильный числовой id при первом появлении.
        """
        with self._begin_write() as txn:
            txn.put(self.dbi(txn, "symbols"), key.encode(),
                    json.dumps(body, ensure_ascii=False).encode())
            self._sym_id_locked(txn, key)

    def map_edges_of(self, caller: str) -> list:
        """callee-дубликаты из call_edges для caller (DUPSORT)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            ce_dbi = self.dbi(txn, "call_edges")
            with txn.cursor(ce_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY, caller.encode())
                while rc == mdbx.RC_SUCCESS:
                    out.append(val.decode())
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_callers_of(self, callee: str) -> list:
        """Обратные рёбра: кто вызывает callee (DUPSORT call_edges_rev)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            ce_dbi = self.dbi(txn, "call_edges_rev")
            with txn.cursor(ce_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY, callee.encode())
                while rc == mdbx.RC_SUCCESS:
                    out.append(val.decode())
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_put_edge(self, caller: str, callee: str, kind: str = "syntax") -> None:
        with self._begin_write() as txn:
            ce_dbi = self.dbi(txn, "call_edges")
            val = json.dumps({"kind": kind, "resolved": False,
                              "ambiguous": False, "callee": callee},
                             ensure_ascii=False).encode()
            with txn.cursor(ce_dbi) as cur:
                cur.put_nodupe(caller.encode(), val)
            rev_dbi = self.dbi(txn, "call_edges_rev")
            with txn.cursor(rev_dbi) as cur:
                cur.put_nodupe(callee.encode(), caller.encode())

    def map_groups_of(self, group_key: str) -> list:
        """Члены группы (DUPSORT-набор)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                rc, _, val = cur.get(mdbx.CURSOR_SET_KEY, group_key.encode())
                while rc == mdbx.RC_SUCCESS:
                    out.append(val.decode())
                    rc, _, val = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def map_group_members(self, prefix: str = "") -> list:
        """Все группы с их членами по префиксу ключа группы."""
        out = []
        with self.env.begin(readonly=True) as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                rc, k, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    gkey = k.decode()
                    if not prefix or gkey.startswith(prefix):
                        out.append((gkey, val.decode()))
                    rc, k, val = cur.get(mdbx.CURSOR_NEXT)
        return out

    def map_put_group(self, group_key: str, member: str) -> None:
        with self._begin_write() as txn:
            gr_dbi = self.dbi(txn, "groups")
            with txn.cursor(gr_dbi) as cur:
                cur.put_nodupe(group_key.encode(), member.encode())

    def map_load_batch(self, symbols: dict, edges: list, groups: dict,
                       chunk: int = 4096, replace: bool = False) -> dict:
        """Батчевая загрузка структурного слоя.

        Принимает готовые данные (из автогенератора) и пишет в
        symbols/call_edges/call_edges_rev/groups.
        При replace=True таблицы очищаются перед загрузкой (самоизлечение
        дрейфа координат: исчезнувшие символы удаляются). Безопасно —
        это перегенерируемый слой, курируемые tables не затрагиваются.
        Возвращает счётчики для сверки.
        """
        put_sym, put_edge, put_grp = 0, 0, 0
        sym_dbi = self._dbi["symbols"]
        ce_dbi = self._dbi["call_edges"]
        rev_dbi = self._dbi["call_edges_rev"]
        gr_dbi = self._dbi["groups"]
        with self._begin_write() as txn:
            if replace:
                for dbi in (sym_dbi, ce_dbi, rev_dbi, gr_dbi):
                    with txn.cursor(dbi) as cur:
                        rc, k, _ = cur.get(mdbx.CURSOR_FIRST)
                        while rc == mdbx.RC_SUCCESS:
                            nxt = cur.get(mdbx.CURSOR_NEXT)
                            cur.delete()
                            rc, k, _ = nxt
            sid_dbi = self._dbi["sym_ids"]
            id2key_dbi = self._dbi["sym_id2key"]
            for i, (key, body) in enumerate(sorted(symbols.items())):
                txn.put(sym_dbi, key.encode(),
                        json.dumps(body, ensure_ascii=False).encode())
                put_sym += 1
                # стабильный числовой id (НЕ чистится при replace)
                rc, val = txn.get(sid_dbi, key.encode())
                if val is None or len(val) != 8:
                    nid = self.next_id
                    self.next_id += 1
                    txn.put(self.dbi(txn, "meta"), b"next_id",
                            _pack_u64(self.next_id))
                    txn.put(sid_dbi, key.encode(), _pack_u64(nid))
                    txn.put(id2key_dbi, _pack_u64(nid), key.encode())
            for e in edges:
                # JSON-значение: kind, resolved, ambiguous, callee
                val = json.dumps({
                    "kind": e.get("kind", "syntax"),
                    "resolved": e.get("resolved", False),
                    "ambiguous": e.get("ambiguous", False),
                    "callee": e["callee"],
                }, ensure_ascii=False).encode()
                with txn.cursor(ce_dbi) as cur:
                    cur.put_nodupe(e["caller"].encode(), val)
                put_edge += 1
                # обратное ребро: callee → caller
                with txn.cursor(rev_dbi) as cur:
                    cur.put_nodupe(e["callee"].encode(), e["caller"].encode())
            for gkey, members in groups.items():
                with txn.cursor(gr_dbi) as cur:
                    for m in members:
                        cur.put_nodupe(gkey.encode(), m.encode())
                        put_grp += 1
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
                        rc, k, _ = cur.get(mdbx.CURSOR_FIRST)
                        while rc == mdbx.RC_SUCCESS:
                            nxt = cur.get(mdbx.CURSOR_NEXT)
                            cur.delete()
                            rc, k, _ = nxt
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
        nid = self.next_id
        self.next_id += 1
        txn.put(self.dbi(txn, "meta"), b"next_id", _pack_u64(self.next_id))
        txn.put(sid_dbi, key.encode(), _pack_u64(nid))
        txn.put(self.dbi(txn, "sym_id2key"), _pack_u64(nid), key.encode())
        return nid

    def stats(self) -> dict:
        by_type = {}
        total_importance = 0.0
        n = 0
        incoming_top = []
        for key, val in self._all_records():
            body = json.loads(val.decode())
            t = body.get("type", "?")
            by_type[t] = by_type.get(t, 0) + 1
            total_importance += body.get("importance", 0.0)
            n += 1
            rec_id = body.get("_id", 0)
            with self.env.begin(readonly=True) as txn:
                inc = self._incoming_count(txn, rec_id)
            incoming_top.append((key.decode(), inc))
        incoming_top.sort(key=lambda kv: -kv[1])
        return {
            "records": n,
            "by_type": by_type,
            "avg_importance": round(total_importance / n, 3) if n else 0.0,
            "top_linked": incoming_top[:10],
            "vocab_modules": len(self.norm.modules),
            "vocab_topics": len(self.norm.topics),
            "last_gc": self.last_gc,
            "next_id": self.next_id,
        }

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        # сбросить накопленные LRU-касания и остановить фоновый тред синка
        try:
            self._flush_touches()
        except Exception:
            pass
        self._stop_sync_thread()
        try:
            self.env.sync(force=True, nonblock=False)
        except Exception:
            pass
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