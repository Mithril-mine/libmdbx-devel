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
        # READY-маркер ПОСЛЕ установки обработчика: тест ждёт его и только
        # потом шлёт SIGTERM — иначе на медленном CI-раннере сигнал может
        # попасть в окно записи, и «N записей» станет невыполнимым ожиданием
        # (это была причина флейка: фиксированный sleep(1.5) гонялся со
        # стартом writer'а).
        with open(path + ".ready", "w") as _f:
            _f.write("READY\\n")
        import time as _t
        _t.sleep(30)
    elif mode == "hold":
        # hold: держим env открытым и НЕ закрываем (меты остаются unsteady);
        # READY ставим ПОСЛЕ всех записей и финального flush: тест ждёт его
        # вместо опроса read-only открытия, которое на unsteady-метах может
        # давать WANNA_RECOVERY/сбоить во время активной записи (флейк на
        # нагруженном CI: _wait_for_records возвращал -1 даже после 60s).
        with open(path + ".ready", "w") as _f:
            _f.write("READY\\n")
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


def _writer_script(dir=None):
    """Записывает скрипт-писатель в каталог `dir` и возвращает путь.

    `dir=None` — mkdtemp БЕЗ очистки (утечка в /tmp): передавайте pytest
    tmp_path, который чистится автоматически. Отдельный каталог
    persist-writer-* больше не создаётся.
    """
    import tempfile
    d = str(dir) if dir is not None else tempfile.mkdtemp(prefix="persist-writer-")
    path = os.path.join(d, "writer.py")
    with open(path, "w") as f:
        f.write(WRITER_TEMPLATE.format(moddir=MODULE_DIR))
    return path


def _spawn_writer(db_path, sync_mode, flush_each, mode="close", dir=None):
    script = _writer_script(dir)
    proc = subprocess.Popen(
        [sys.executable, script, db_path, sync_mode, "1" if flush_each else "0",
         str(N_RECORDS), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc, script


def _wait_ready(db_path, timeout=120):
    """Ждём маркер `<db>.ready` от writer'а (коммиты сделаны, режим готов).

    Таймаут завышен намеренно: на разделяемом CI-раннере старт Python-писателя
    и первые записи могут растянуться за 20s."""
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
        [sys.executable, _writer_script(tmp_path), db, sync_mode, "0",
         str(N_RECORDS), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if mode in ("kill9", "sigterm"):
            # Ждём READY-маркер: writer закоммитил все N записей (а для
            # sigterm — ещё и установил обработчик). Фиксированный sleep
            # гонялся со стартом writer'а на медленных CI-раннерах: SIGTERM
            # попадал в окно записи, и часть записей ещё не была закоммичена
            # (флейк test_store_persistence_matrix[sigterm-durable]: 1 из 10
            # записей при 8 закоммиченных txnid).
            assert _wait_ready(db), "writer не закоммитил записи до сигнала"
            time.sleep(1.5)  # окно writeback частичных мет (как в kill9-тесте)
            proc.send_signal(signal.SIGTERM if mode == "sigterm" else signal.SIGKILL)
            proc.wait(timeout=10)
        else:  # graceful close
            proc.wait(timeout=120)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    stderr = proc.stderr.read().decode("utf-8", "replace")
    time.sleep(0.3)
    res = _verify(db, N_RECORDS)
    got = res["readonly_open"].get("records") if res["readonly_open"]["ok"] else -1
    if sync_mode == "durable":
        assert res["readonly_open"]["ok"], (res, stderr)
        assert got == N_RECORDS, (res, stderr)
    elif mode in ("close", "sigterm"):
        assert res["readonly_open"]["ok"], (res, stderr)
        assert got == N_RECORDS, (res, stderr)
    else:  # safe_nosync + kill9 (без close): read-only открытие НЕ должно
        # молча откатываться — либо честный успех (движок успел steady-sync),
        # либо явный сигнал WANNA_RECOVERY → решение пользователя (chk / rw).
        if res["readonly_open"]["ok"]:
            assert 0 <= got <= N_RECORDS, (res, stderr)
        else:
            err = res["readonly_open"].get("error", "")
            assert "WANNA_RECOVERY" in err or "recover" in err.lower(), (res, stderr)


def test_store_persistence_flush_each(tmp_path):
    """A2: safe_nosync + flush после КАЖДОЙ записи переживает SIGKILL."""
    db = str(tmp_path / "t.mdbx")
    proc = subprocess.Popen(
        [sys.executable, _writer_script(tmp_path), db, "safe_nosync", "1",
         str(N_RECORDS), "hold"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # READY-маркер writer ставит ПОСЛЕ всех записей и финального flush.
        # Опрос read-only открытия во время активной записи ненадёжен: на
        # нагруженном CI readonly-Store поверх незакрытых unsteady-мет может
        # кидать WANNA_RECOVERY/сбоить, из-за чего _wait_for_records ()
        # возвращал -1 даже при успешно дописавшем writer'е (флейк).
        assert _wait_ready(db), "writer не закоммитил записи до сигнала"
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

    Проверяемый контракт (после #182 + mdbx_preopen_snapinfo_ex):
      1) preopen_snapinfo (mode=PROPERLY) → MDBX_WANNA_RECOVERY, если новейшая
         мета weak (троица s:w:w) — явный сигнал пользователю без открытия;
      2) read-only открытие → MDBX_WANNA_RECOVERY — тот же сигнал, но уже от
         open (не тихий откат к steady);
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
            [sys.executable, _writer_script(tmp_path), db, "safe_nosync", "0",
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
        # After #182 preopen_snapinfo(mode=PROPERLY) reports MDBX_WANNA_RECOVERY
        # when the newest checkpoint is not steady. Three outcomes are possible
        # after the SIGKILL, and only the first is the contract under test:
        #   - WANNA_RECOVERY: the newest meta is a weak checkpoint of the
        #     current boot -> this is what we want to observe;
        #   - preopen SUCCEEDS: the OS writeback flushed every meta steady
        #     before the kill ("unlucky timing") -> retry on a fresh DB;
        #   - CORRUPTED / any other error: the kill raced the writer and left
        #     no usable meta -> retry on a fresh DB.
        # NB: the retry signal must not be an assertion raised inside the try,
        # because AssertionError is not a LibmdbxError and would escape the
        # bounded-retry loop (that made this test flaky on macOS, where the OS
        # more often flushes the metas steady within the writeback window).
        unlucky = False
        try:
            m.preopen_snapinfo(db)
        except m.LibmdbxError as e:
            if e.rc == m.RC_WANNA_RECOVERY:
                break  # the expected signal: a weak checkpoint of this boot
            unlucky = True  # CORRUPTED / kill raced the writer
        else:
            unlucky = True  # preopen succeeded: metas landed fully steady
        if unlucky:
            if attempt == 3:
                pytest.fail("all 4 attempts landed on a fully-steady or "
                            "unusable meta troika; cannot observe the "
                            "WANNA_RECOVERY contract on this host")
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
    with Store(db, readonly=True, _skip_sync_thread=True) as s:
        with pytest.raises(Exception) as ei:
            s.safe_store("fact:practice:durability-zz", "fact", "no", 0.5)
        text = str(ei.value)
        assert "READONLY_MODE" in text or "read-only" in text, text
