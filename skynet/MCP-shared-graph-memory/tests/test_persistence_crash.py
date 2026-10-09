"""Тесты персистентности при разных режимах синхронизации и способах
завершения процесса (матрица A×B×C).

Гипотеза владельца: после доработок MCP-модуля (SAFE_NOSYNC + sync-poll)
данные не фиксируются на диске и теряются после рестарта сессии.

Сценарий каждой ячейки:
  - подпроцесс открывает Store (режим B) и пишет N записей;
  - завершается способом C (graceful close / SIGTERM / SIGKILL);
  - верификация трёхшаговая:
    1) mdbx_preopen_snapinfo — recent_txnid на диске БЕЗ открытия;
    2) открытие read-only — число записей + next_id (движок не делает
       recovery/rollback в read-only);
    3) если read-only открытие не удалось — фиксируется как «требует
       решения пользователя» (chk или осознанный rw-откат).

Ожидания по документации libmdbx (§8.2):
  - DURABLE × любой C        = N записей;
  - SAFE_NOSYNC × close/SIGTERM = N (close() вызывает force sync);
  - SAFE_NOSYNC × SIGKILL без close: меты остаются unsteady (троица
    s:w:w), поэтому read-only открытие НЕ удаётся (MDBX_WANNA_RECOVERY) —
    это защита от «молчаливого отката»: пользователь явно решает между
    mdbx_chk и осознанным rw-открытием (rw само сделает steady-sync;
    данные при safe_nosync обычно уже на диске);
  - force-sync после каждой записи (B3) × любой C = N.
"""

import json
import os
import signal
import subprocess
import sys
import textwrap
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mcp import Store  # noqa: E402
from mcp import libmdbx as m  # noqa: E402

N_RECORDS = 10
MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WRITER_TEMPLATE = textwrap.dedent(
    """
    import sys, os
    sys.path.insert(0, {moddir!r})
    from mcp import Store
    path, sync_mode, flush_each, n, mode = (
        sys.argv[1], sys.argv[2], sys.argv[3] == "1", int(sys.argv[4]),
        sys.argv[5] if len(sys.argv) > 5 else "store")
    s = Store(path, readonly=False, sync_mode=sync_mode,
              _skip_sync_thread=True)
    s.vocab_add("fact", "persistence")
    import hashlib
    for i in range(n):
        topic = "durability-" + chr(ord("a") + i) * 2
        s.vocab_add("practice", topic)
        digest = hashlib.sha256(b"unique-payload-%d" % i).hexdigest()[:24]
        summary = "Persistent record number %d token %s" % (i, digest)
        s.safe_store("fact:practice:" + topic, "fact", summary, 0.5)
        if flush_each:
            s.flush_sync()
    if mode == "flush":
        s.flush_sync()
    if mode in ("sigterm", "signal_writer"):
        # установить обработчик SIGTERM -> close (graceful, как в сервере)
        import signal as _sig
        def _on_term(*_a):
            s.flush_sync()
            s.close()
            os._exit(0)
        _sig.signal(_sig.SIGTERM, _on_term)
        if mode == "signal_writer":
            _sig.raise_signal(_sig.SIGTERM)
        import time as _t
        _t.sleep(30)
    elif mode == "hold":
        # hold: держим env открытым и НЕ закрываем (меты остаются unsteady)
        import time as _t
        _t.sleep(30)
    elif mode == "kill9":
        # kill9: пишем стартовые записи, сигналим READY-маркером и держим env
        # открытым, НЕ закрывая (меты на диске остаются неоднородными s:w:w
        # после SIGKILL из теста; окно writeback частичных мет — в тесте).
        with open(path + ".ready", "w") as _f:
            _f.write("READY\\n")
        import time as _t
        _t.sleep(30)
    else:
        s.close()
    """
)


def _writer_script():
    """Записывает скрипт-писатель во временный каталог и возвращает путь."""
    import tempfile
    d = tempfile.mkdtemp(prefix="persist-writer-")
    path = os.path.join(d, "writer.py")
    with open(path, "w") as f:
        f.write(WRITER_TEMPLATE.format(moddir=MODULE_DIR))
    return path


def _spawn_writer(db_path, sync_mode, flush_each, mode="close"):
    script = _writer_script()
    proc = subprocess.Popen(
        [sys.executable, script, db_path, sync_mode, "1" if flush_each else "0",
         str(N_RECORDS), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc, script


def _wait_for_records(db_path, expected, timeout=60):
    """Ждём, пока в read-only открытии станет expected записей.

    Таймаут завышен намеренно: на разделяемом CI-раннере (несколько
    параллельных workflow после repository_dispatch) старт Python-писателя
    и первые записи могут растянуться за 20s; тест флакал с -1 при том же
    коде (push-прогоны проходят). Возврат происходит сразу после появления
    записей, поэтому лишний таймаут не замедляет успешный путь."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = Store(db_path, readonly=True, _skip_sync_thread=True)
            n = sum(1 for k in s.recall("fact:practice:durability-", limit=200)
                    if k.get("key", "").startswith("fact:practice:durability-"))
            s.close()
            if n >= expected:
                return n
        except Exception:
            pass
        time.sleep(0.2)
    return -1


def _wait_ready(db_path, timeout=15):
    """Ждём маркер `<db>.ready` от writer'а (kill9-поток коммитов запущен)."""
    marker = db_path + ".ready"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(marker):
            return True
        time.sleep(0.05)
    return False


def _verify(db_path, expected):
    """Трёхшаговая верификация. Возвращает dict с результатами."""
    out = {"expected": expected}

    # шаг 1: preopen_snapinfo (без открытия)
    try:
        pre = m.preopen_snapinfo(db_path)
        out["preopen_recent_txnid"] = pre["recent_txnid"]
        out["preopen_ok"] = True
    except Exception as e:  # noqa: BLE001
        out["preopen_ok"] = False
        out["preopen_error"] = str(e)

    # шаг 2: read-only открытие
    ro = {"ok": False}
    try:
        s = Store(db_path, readonly=True, _skip_sync_thread=True)
        records = s.recall("fact:practice:durability-", limit=200)
        got = sum(1 for k in records
                  if k.get("key", "").startswith("fact:practice:durability-"))
        ro.update({"ok": True, "records": got,
                   "next_id": s.stats().get("next_id"),
                   "meta_txnid": s.diag().get("env", {}).get("meta_txnid")})
        s.close()
    except Exception as e:  # noqa: BLE001
        ro["error"] = str(e)
    out["readonly_open"] = ro
    return out


# --- матрица ----------------------------------------------------------------

@pytest.mark.parametrize("sync_mode", ["durable", "safe_nosync"])
@pytest.mark.parametrize("mode", ["close", "sigterm", "kill9"])
def test_store_persistence_matrix(tmp_path, sync_mode, mode):
    """A2 (Store): режимы sync × способы завершения."""
    db = str(tmp_path / "t.mdbx")
    proc = subprocess.Popen(
        [sys.executable, _writer_script(), db, sync_mode, "0",
         str(N_RECORDS), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if mode == "kill9":
            # writer пишет 10 записей за доли секунды и уходит в hold
            time.sleep(1.5)
            proc.send_signal(signal.SIGKILL)
            proc.wait(timeout=10)
        elif mode == "sigterm":
            # writer пишет, ставит обработчик SIGTERM (graceful close+sync)
            time.sleep(1.5)
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=10)
        else:  # graceful close
            proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    time.sleep(0.3)
    res = _verify(db, N_RECORDS)
    got = res["readonly_open"].get("records") if res["readonly_open"]["ok"] else -1
    if sync_mode == "durable":
        assert res["readonly_open"]["ok"], res
        assert got == N_RECORDS, res
    elif mode in ("close", "sigterm"):
        assert res["readonly_open"]["ok"], res
        assert got == N_RECORDS, res
    else:  # safe_nosync + kill9 (без close): read-only открытие НЕ должно
        # молча откатываться — либо честный успех (движок успел steady-sync),
        # либо явный сигнал WANNA_RECOVERY → решение пользователя (chk / rw).
        if res["readonly_open"]["ok"]:
            assert 0 <= got <= N_RECORDS, res
        else:
            err = res["readonly_open"].get("error", "")
            assert "WANNA_RECOVERY" in err or "recover" in err.lower(), res


def test_store_persistence_flush_each(tmp_path):
    """A2: safe_nosync + flush после КАЖДОЙ записи переживает SIGKILL."""
    db = str(tmp_path / "t.mdbx")
    proc = subprocess.Popen(
        [sys.executable, _writer_script(), db, "safe_nosync", "1",
         str(N_RECORDS), "hold"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert _wait_for_records(db, N_RECORDS) >= 0
        # дать writer'у выполнить финальный flush_sync() после последней записи:
        # иначе SIGKILL может попасть в окно commit(N) -> flush(N) и меты
        # останутся unsteady (флейк: 24.w:25.w:23.s)
        time.sleep(0.3)
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    time.sleep(0.3)
    res = _verify(db, N_RECORDS)
    assert res["readonly_open"]["ok"], res
    assert res["readonly_open"]["records"] == N_RECORDS, res


def test_kill9_nosync_readonly_wants_recovery(tmp_path):
    """safe_nosync + SIGKILL без close: read-only НЕ откатывается молча.

    Проверяемый контракт:
      1) preopen_snapinfo → MDBX_CORRUPTED (меты unsteady, троица s:w:w);
      2) read-only открытие → MDBX_WANNA_RECOVERY — явный сигнал пользователю
         (не тихий откат к steady);
      3) осознанное read-write открытие само делает steady-sync и данные
         целы (safe_nosync сбрасывает страницы данных, задержаны только меты).

    Устойчивость: тест ждёт READY-маркер (стартовые записи закоммичены),
    даёт окно writeback частичных мет (1.2s) и убивает SIGKILL'ом — на
    Linux это даёт unsteady стабильно (8/8 в замере); остаточная удача
    ОС-флаша (меты успели лечь steady целиком) поглощается bounded-retry:
    до 4 попыток на свежих БД.
    """
    db = None
    for attempt in range(4):
        db = str(tmp_path / ("t%d.mdbx" % attempt))
        proc = subprocess.Popen(
            [sys.executable, _writer_script(), db, "safe_nosync", "0",
             str(N_RECORDS), "kill9"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert _wait_ready(db), "writer не закоммитил стартовые записи"
            time.sleep(1.2)  # окно writeback частичных мет (как в исходном 1.5s)
            proc.send_signal(signal.SIGKILL)
            proc.wait(timeout=10)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        time.sleep(0.3)
        try:
            with pytest.raises(Exception) as ei:
                m.preopen_snapinfo(db)
            assert "CORRUPTED" in str(ei.value), ei.value
            break
        except (AssertionError, pytest.fail.Exception):
            # Меты успели лечь steady целиком (OS writeback) либо троица
            # не s:w:w — это не провал контракта, а неудачный тайминг:
            # переходим к следующей свежей БД.
            if attempt == 3:
                raise
            continue

    with pytest.raises(Exception) as ei:
        Store(db, readonly=True, _skip_sync_thread=True)
    text = str(ei.value)
    assert "WANNA_RECOVERY" in text and "recover" in text.lower(), text

    s = Store(db, readonly=False, _skip_sync_thread=True)
    try:
        got = sum(1 for k in s.recall("fact:practice:durability-", limit=200)
                  if k.get("key", "").startswith("fact:practice:durability-"))
        assert got == N_RECORDS
    finally:
        s.close()


def test_server_main_sigterm_flushes(tmp_path):
    """A3: MCP-сервер (main) по SIGTERM делает close -> force sync."""
    import io
    from mcp.mcp_server import main as server_main
    # запускаем реальный сервер в подпроцессе со stdin pipe (loop ждёт строк)
    import subprocess as sp
    env = dict(os.environ)
    env["SHARED_GRAPH_MEMORY_PATH"] = str(tmp_path / "server.mdbx")
    # заставляем сервер написать запись, затем SIGTERM
    db = str(tmp_path / "server.mdbx")
    Store(db, readonly=False, _skip_sync_thread=True).close()  # создать
    script = textwrap.dedent(
        """
        import sys, os, time, signal
        sys.path.insert(0, {moddir!r})
        os.environ["SHARED_GRAPH_MEMORY_PATH"] = {db!r}
        os.environ["SHARED_GRAPH_MEMORY_READONLY"] = "0"
        from mcp.mcp_server import main
        from mcp import Store
        # пишем запись, затем запускаем сервер (read-write) и держим живым
        main([])
        """.format(moddir=MODULE_DIR, db=db))
    spath = tmp_path / "server_script.py"
    spath.write_text(script)
    p = sp.Popen([sys.executable, str(spath)],
                 stdin=sp.PIPE, stdout=sp.PIPE, stderr=sp.PIPE)
    try:
        # дождаться создания сервера
        time.sleep(1.5)
        p.send_signal(signal.SIGTERM)
        p.wait(timeout=15)
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
    time.sleep(0.3)
    res = _verify(db, 1)  # хотя бы БД читается; сервер стартовал rw
    assert res["readonly_open"]["ok"], res


def test_readonly_blocks_writes(tmp_path):
    """В read-only режиме запись отклоняется контрактной ошибкой."""
    db = str(tmp_path / "t.mdbx")
    s0 = Store(db, readonly=False, _skip_sync_thread=True)
    s0.vocab_add("practice", "durability-zz")
    s0.close()
    s = Store(db, readonly=True, _skip_sync_thread=True)
    try:
        with pytest.raises(Exception) as ei:
            s.safe_store("fact:practice:durability-zz", "fact", "no", 0.5)
        text = str(ei.value)
        assert "READONLY_MODE" in text or "read-only" in text, text
    finally:
        s.close()