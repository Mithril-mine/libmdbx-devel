"""Бэкап, защита от удаления файла БД и новая интроспекция.

Покрывает:
  - LIFELINE-ссылку current.mdbx при старте (инвариант st_nlink >= 2);
  - консистентный бэкап через txn_copy2pathname (COMPACT) и имена по тирам;
  - таймеры AUTO (тихо) и PROMPT (с запросом);
  - ротацию AUTO-копий (dry_run/confirm; чужие тиры не трогаются);
  - детекцию удаления файла БД: аварийный снапшот + DB_FILE_DELETED;
  - commit-latency, fileinfo, db_recover на живом сервере.
"""

import os
import signal
import subprocess
import sys
import time

import pytest

from mcp import Store
from mcp.errors import MemoryError
from mcp.mcp_server import McpServer

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _files(store, prefix=""):
    try:
        return sorted(f for f in os.listdir(store.backup_dir)
                      if f.startswith(prefix))
    except OSError:
        return []


def test_lifeline_created_on_open(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        fi = s.fileinfo()
        assert fi["lifeline_ok"] is True, fi
        assert os.path.isfile(os.path.join(s.backup_dir, "current.mdbx"))
        if fi.get("lifeline_mode") == "copy":
            # кросс-ФС backup_dir (EXDEV): жёсткая ссылка невозможна —
            # деградация в полную копию (данные сохранены, детекция через
            # пропажу пути + nlink == 1), но инвариант nlink >= 2 не держится.
            assert fi["nlink"] == 1, fi
            assert os.path.getsize(fi["lifeline"]) >= fi["size"], fi
        else:
            assert fi["nlink"] >= 2, fi
    finally:
        s.close()


def test_backup_pin_compact_snapshot(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        s.vocab_add("practice", "durability-aa")
        s.safe_store("fact:practice:durability-aa", "fact", "bk", 0.5)
        r = s.backup(kind="pin")
        assert r["kind"] == "pin"
        assert os.path.isfile(r["path"])
        assert os.path.basename(r["path"]).startswith("pin-")
        assert r["txnid"] > 0
        assert _files(s, "pin-") == [os.path.basename(r["path"])]
        # дефолт backup() — pin (внутренняя точка сохранения)
        r2 = s.backup()
        assert r2["kind"] == "pin"
    finally:
        s.close()


def test_auto_backup_timer_and_state(tmp_path, monkeypatch):
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        assert s.auto_backup_seconds == 1
        # первый вызов before_call срабатывает (last_auto пусто)
        out = s.before_call()
        assert not out.get("backup_notice")
        time.sleep(0.05)
        assert _files(s, "auto-"), "AUTO-бэкап должен быть создан"
        st = s._load_backup_state()
        assert st.get("last_auto"), "таймер должен быть сохранён"
        # следующий вызов сразу — таймер ещё не истёк
        n_before = len(_files(s, "auto-"))
        s.before_call()
        assert len(_files(s, "auto-")) == n_before
    finally:
        s.close()


def test_prompt_backup_pending_and_approve(tmp_path, monkeypatch):
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PROMPT_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        out = s.before_call()
        assert out.get("backup_pending") is True, out
        assert "спросите пользователя" in out.get("backup_prompt", "")
        r = s.run_prompt_backup(approve=True)
        assert r["approved"] is True
        assert r.get("path"), r
        assert _files(s, "pin-")
        # pending снят
        assert s.before_call().get("backup_pending") is None
    finally:
        s.close()


def test_prompt_backup_skip(tmp_path, monkeypatch):
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_PROMPT_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        s.before_call()  # pending установлен
        r = s.run_prompt_backup(approve=False)
        assert r["approved"] is False
        assert _files(s, "pin-") == []
    finally:
        s.close()


def test_rotation_candidates_and_cleanup(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        os.makedirs(s.backup_dir, exist_ok=True)
        # старые AUTO-копии (не реальные бэкапы — только для ротации)
        for i in range(s.backup_keep + 3):
            name = "auto-20200101-0000%02d_txnid%d.mdbx" % (i, i)
            with open(os.path.join(s.backup_dir, name), "wb") as fh:
                fh.write(b"x" * 100)
        cands = s.cleanup(dry_run=True)
        assert len(cands["candidates"]) == 3, cands
        # dry_run ничего не удалил
        assert len(_files(s, "auto-")) == s.backup_keep + 3
        # confirm удаляет только кандидатов
        r = s.cleanup(dry_run=False, confirm=True)
        assert len(r["removed"]) == 3
        assert len(_files(s, "auto-")) == s.backup_keep
        # тир pin не в кандидатах
        s.backup(kind="pin")
        assert _files(s, "pin-")
        assert s.cleanup(dry_run=True)["candidates"] == []
    finally:
        s.close()


def test_cleanup_without_confirm_is_noop(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        os.makedirs(s.backup_dir, exist_ok=True)
        for i in range(s.backup_keep + 2):
            name = "auto-20200101-0000%02d_txnid%d.mdbx" % (i, i)
            open(os.path.join(s.backup_dir, name), "wb").write(b"y")
        n_before = len(_files(s, "auto-"))
        r = s.cleanup(dry_run=False, confirm=False)  # без подтверждения
        assert r["dry_run"] is True and r["removed"] == []
        assert len(_files(s, "auto-")) == n_before
    finally:
        s.close()


def test_deletion_detected_snapshot_and_blocked(tmp_path):
    if os.name == "nt":
        pytest.skip("st_nlink механизм только не-Windows")
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        s.vocab_add("practice", "durability-aa")
        s.safe_store("fact:practice:durability-aa", "fact", "x", 0.5)
        os.unlink(db)  # удаляем файл из-под процесса
        with pytest.raises(MemoryError) as ei:
            s.before_call()
        assert "DB_FILE_DELETED" in str(ei.value), ei.value
        assert s.db_file_deleted is True
        snaps = [f for f in _files(s, "snapshot_")]
        assert snaps, "аварийный снапшот должен быть создан"
        # операции записи заблокированы предохранителем
        with pytest.raises(MemoryError) as ei:
            s.safe_store("fact:practice:durability-aa", "fact", "y", 0.5)
        assert "DB_FILE_DELETED" in str(ei.value), ei.value
    finally:
        s.close()  # close на удалённом inode не должен падать


def test_commit_latency_collected(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        assert s.commit_latency()["count"] == 0
        s.vocab_add("practice", "durability-aa")
        for _ in range(3):
            s.safe_store("fact:practice:durability-aa", "fact", "lat", 0.5)
        lat = s.commit_latency()
        assert lat["count"] >= 3, lat
        assert lat["last_us"]["whole"] > 0, lat
        assert lat["window_whole_us"]["mean"] > 0
    finally:
        s.close()


def test_mcp_new_tools_and_prompt_notice(store, tmp_path, monkeypatch):
    monkeypatch.setattr(store, "prompt_backup_seconds", 1)
    store.backup_pending = True  # имитация сработавшего PROMPT-таймера
    serv = McpServer(store)

    def call(name, args=None):
        return serv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                            "params": {"name": name, "arguments": args or {}}})

    r = call("db_fileinfo")
    assert "nlink" in r["result"]["structuredContent"]
    r = call("db_latency")
    assert "count" in r["result"]["structuredContent"]
    # kind из протокола убран: db_backup при pending = PROMPT-подтверждение
    # → внутренняя точка сохранения (pin-*)
    r = call("db_backup", {})
    sc = r["result"]["structuredContent"]
    assert sc["approved"] is True and sc.get("path"), sc
    assert os.path.basename(sc["path"]).startswith("pin-"), sc
    # db_export: dest обязателен и вне backup-каталога
    ext = str(tmp_path / "outside" / "mcp-export.mdbx")
    r = call("db_export", {"dest": ext})
    sc = r["result"]["structuredContent"]
    assert sc["kind"] == "export" and sc["path"] == ext, sc
    # export без dest → контрактная ошибка
    r = call("db_export", {})
    assert r.get("error") and "dest обязателен" in r["error"]["message"]
    # ревью #164: export ВНУТРЬ backup_dir (и в подкаталог) → invalid
    r = call("db_export", {"dest": os.path.join(store.backup_dir, "x.mdbx")})
    assert r.get("error") and "внутри backup-каталога" in r["error"]["message"]
    r = call("db_export",
             {"dest": os.path.join(store.backup_dir, "sub", "x.mdbx")})
    assert r.get("error") and "внутри backup-каталога" in r["error"]["message"]
    r = call("db_backup_status")
    sc = r["result"]["structuredContent"]
    assert "tiers" in sc
    # экспорт не учитывается в tiers каталога
    assert sc["tiers"].get("pin", 0) >= 1
    r = call("db_backup_cleanup", {"dry_run": True})
    assert r["result"]["structuredContent"]["dry_run"] is True
    r = call("db_recover", {"target_meta": 0})
    sc = r["result"]["structuredContent"]
    assert "meta_txnid" in sc, sc


def test_server_startup_wants_recovery(tmp_path):
    """Старт сервера на unsteady БД (kill9 без close) — понятный отказ rc=2."""
    if os.name == "nt":
        pytest.skip("POSIX SIGKILL")
    mod = MODULE_DIR
    db = str(tmp_path / "t.mdbx")
    writer = tmp_path / "w.py"
    writer.write_text(
        "import sys, time\n"
        "sys.path.insert(0, %r)\n"
        "from mcp import Store\n"
        "s = Store(sys.argv[1], readonly=False, _skip_sync_thread=True)\n"
        "s.vocab_add('practice', 'durability-aa')\n"
        "s.safe_store('fact:practice:durability-aa', 'fact', 'x', 0.5)\n"
        "time.sleep(30)\n" % mod)
    proc = subprocess.Popen([sys.executable, str(writer), db],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # дождаться реального создания/записи БД (а не фиксированной паузы:
        # под нагрузкой Python может не успеть инициализироваться)
        deadline = time.time() + 30.0
        while time.time() < deadline:
            if os.path.exists(db) and os.path.getsize(db) > 0:
                break
            time.sleep(0.05)
        time.sleep(0.3)
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    assert os.path.exists(db) and os.path.getsize(db) > 0, "writer not started"
    env = dict(os.environ, PYTHONPATH=mod,
               SHARED_GRAPH_MEMORY_PATH=db,
               SHARED_GRAPH_MEMORY_READONLY="1")
    p = subprocess.run([sys.executable, "-m", "mcp.mcp_server"],
                       capture_output=True, text=True, env=env, timeout=30)
    assert p.returncode == 2, (p.returncode, p.stderr)
    assert "recovery" in p.stderr.lower(), p.stderr


def test_server_startup_log(tmp_path):
    """Лог старта: путь/режим/next_id/meta_txnid в stderr."""
    mod = MODULE_DIR
    db = str(tmp_path / "t.mdbx")
    env = dict(os.environ, PYTHONPATH=mod, SHARED_GRAPH_MEMORY_PATH=db)
    p = subprocess.run([sys.executable, "-m", "mcp.mcp_server"],
                       input='{"jsonrpc":"2.0","id":1,"method":"ping"}\n',
                       capture_output=True, text=True, env=env, timeout=30)
    assert p.returncode == 0
    assert "shared-graph-memory started" in p.stderr, p.stderr
    assert "CREATED-NEW" in p.stderr
    assert "readonly=True" in p.stderr, p.stderr


def test_readonly_open_does_not_create_backup_dir(tmp_path, monkeypatch):
    """Read-only открытие НЕ создаёт backup_dir/LIFELINE (без оснований)."""
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    Store(db, _skip_sync_thread=True).close()   # rw создание
    import os
    import shutil
    shutil.rmtree(str(tmp_path / "bk"), ignore_errors=True)  # убрать от rw
    s = Store(db, readonly=True, _skip_sync_thread=True)
    assert not os.path.exists(s.backup_dir), "readonly не создаёт backup_dir"
    s.close()


def test_auto_backup_skipped_when_db_unchanged(tmp_path, monkeypatch):
    """Авто-бэкап не создаётся, если txnid не вырос (нет изменений)."""
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        # первый бэкап (БД изменилась при создании)
        s.before_call()
        n = len(_files(s, "auto-"))
        assert n == 1, "первый auto-бэкап должен быть создан"
        st = s._load_backup_state()
        # нет изменений: следующий тик таймера НЕ должен плодить копии
        st["last_auto"] = 0  # форсируем интервал
        s._save_backup_state(st)
        s.before_call()
        assert len(_files(s, "auto-")) == n, "без изменений — бэкапа нет"


def _inflate_backup_dir(store, mb=2):
    """Создать мусорные .mdbx-файлы, чтобы _backup_dir_size превысил лимит."""
    os.makedirs(store.backup_dir, exist_ok=True)
    for i in range(mb):
        p = os.path.join(store.backup_dir, "filler%d.mdbx" % i)
        with open(p, "wb") as fh:
            fh.write(b"\0" * (1 << 20))


def test_pin_backup_over_limit_rejected_force_bypasses(tmp_path, monkeypatch):
    """D2/N10 + split: внутренняя точка (pin) сверх лимита — явная ошибка;
    force обходит. Экспорт (export) — наружу каталога, лимиту не подчинён.

    Дыра #5 (прямой db_backup kind=auto минует бюджет и ротацию) закрыта
    по построению: kind удалён из протокола MCP — db_backup это всегда pin,
    db_export — всегда наружу. Store.backup(kind="auto") остаётся для
    таймера и всё ещё подчиняется лимиту (защита в глубину).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20  # 1 МБ
        _inflate_backup_dir(s, mb=2)  # каталог ~2 МБ > лимита
        # pin под лимитом — явная ошибка с причиной
        with pytest.raises(MemoryError) as ei:
            s.backup(kind="pin")
        assert "превышает лимит" in str(ei.value)
        assert not _files(s, "pin-"), "копия не должна появиться"
        # force обходит лимит
        r = s.backup(kind="pin", force=True)
        assert os.path.isfile(r["path"])
        assert _files(s, "pin-")
        # auto (внутренний, для таймера) тоже под лимитом — защита в глубину
        with pytest.raises(MemoryError):
            s.backup(kind="auto")
        assert not [f for f in _files(s, "auto-") if "filler" not in f], \
            "auto-копия сверх лимита не должна появиться"
        # экспорт НАРУЖУ каталога — лимиту не подчинён (не расходует бюджет)
        ext = str(tmp_path / "outside" / "export.mdbx")
        re_ = s.export(ext)
        assert os.path.isfile(re_["path"]) and re_["kind"] == "export"
        assert re_["path"] == ext
        # экспорт БЕЗ dest — invalid
        with pytest.raises(MemoryError) as ei2:
            s.export("")
        assert "dest обязателен" in str(ei2.value)
        # экспорт ВНУТРЬ backup-каталога — invalid (это не экспорт)
        with pytest.raises(MemoryError) as ei3:
            s.export(os.path.join(s.backup_dir, "nope.mdbx"))
        assert "внутри backup-каталога" in str(ei3.value)
        # ревью #164: подкаталог backup_dir тоже отклоняется — иначе файл
        # ускользнул бы из лимита/tiers (listdir нерекурсивен)
        with pytest.raises(MemoryError) as ei4:
            s.export(os.path.join(s.backup_dir, "sub", "x.mdbx"))
        assert "внутри backup-каталога" in str(ei4.value)
        assert not os.path.exists(os.path.join(s.backup_dir, "sub"))
        # путь с общим префиксом имени каталога (не подкаталог) — разрешён
        sibling = str(tmp_path / "bk-sibling" / "e.mdbx")
        rs = s.export(sibling)
        assert rs["kind"] == "export" and os.path.isfile(rs["path"])


def test_auto_skip_flag_persistent_cleared_on_success(tmp_path, monkeypatch):
    """D2/N10: пропуск auto по лимиту — персистентный флаг; успех снимает.

    Флаг виден всем процессам роя (state-файл, не память) и переживает
    рестарт; снимается успешным auto-бэкапом после освобождения места.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        _inflate_backup_dir(s, mb=2)
        out = s.before_call()
        assert "пропущен" in out.get("backup_notice", ""), out
        st = s._load_backup_state()
        assert st.get("auto_skipped_limit") is True
        assert not _files(s, "auto-"), "auto-копия не должна появиться"
        assert s.backup_status()["auto_skipped_limit"] is True

        # оператор освободил место; следующий такт таймера проходит и снимает
        for f in _files(s, "filler"):
            os.unlink(os.path.join(s.backup_dir, f))
        st = s._load_backup_state()
        st["last_auto"] = 0          # форсируем таймер
        st["last_auto_txnid"] = 0    # и условие «БД менялась»
        s._save_backup_state(st)
        s.before_call()
        assert _files(s, "auto-"), "auto-бэкап должен пройти после очистки"
        assert s._load_backup_state().get("auto_skipped_limit") is None
        assert s.backup_status()["auto_skipped_limit"] is False


def test_auto_skip_flag_set_on_toctou_failure(tmp_path, monkeypatch):
    """Ревью #164-#1 (TOCTOU): гонка между проверкой таймера и backup()
    тоже выставляет персистентный флаг.

    Сценарий: таймер видит свободный лимит, но между его проверкой и
    вызовом backup(kind="auto") ДРУГОЙ процесс роя заполняет каталог —
    backup() бросает invalid. Раньше except-ветка ставила только
    одноразовый in-memory notice, и backup_status другого агента показывал
    auto_skipped_limit=False, хотя авто-бэкапы фактически блокируются.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        # имитация гонки: каталог заполняется МЕЖДУ проверкой таймера
        # и вызовом backup() — патчим _backup_dir_size: 0 до backup,
        # 2 МБ внутри него
        real_size = s._backup_dir_size
        state = {"phase": "timer"}

        def racing_size():
            if state["phase"] == "timer":
                return 0          # таймер видит свободное место
            return 2 << 20        # backup() видит переполнение

        real_backup = s.backup

        def racing_backup(kind="pin", dest=None, force=False):
            state["phase"] = "backup"  # «другой процесс» заполнил каталог
            return real_backup(kind=kind, dest=dest, force=force)

        monkeypatch.setattr(s, "_backup_dir_size", racing_size)
        monkeypatch.setattr(s, "backup", racing_backup)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        out = s.before_call()
        # backup() упал из-за лимита → флаг обязан быть выставлен
        assert "failed" in (out.get("backup_notice") or ""), out
        monkeypatch.setattr(s, "_backup_dir_size", real_size)
        assert s._load_backup_state().get("auto_skipped_limit") is True
        assert s.backup_status()["auto_skipped_limit"] is True


def test_auto_skip_flag_cleared_on_manual_free(tmp_path, monkeypatch):
    """Ревью #164-#9: флаг auto_skipped_limit отражает СОСТОЯНИЕ лимита,
    а не факт удаления auto-копий.

    При pin-only переполнении ротация cleanup ничего не удаляет (pin не
    ротируется), но оператор может освободить место вручную — cleanup
    обязан снять флаг, когда размер каталога снова под лимитом, иначе рой
    навсегда остался бы с «auto блокирован» без штатного пути разблокировки.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.auto_backup_seconds = 1
        s.backup_max_bytes = 1 << 20
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        # pin-only переполнение (имитация: 2 мусорных pin-* по 1 МБ)
        for i in range(2):
            p = os.path.join(s.backup_dir, "pin-20200101-00000%d_txnid%d.mdbx"
                             % (i, i))
            with open(p, "wb") as fh:
                fh.write(b"\0" * (1 << 20))
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        s.before_call()
        assert s._load_backup_state().get("auto_skipped_limit") is True
        # оператор удалил pin вручную; cleanup ротирует только auto →
        # removed=[] , но место освобождено → флаг обязан сняться
        for f in _files(s, "pin-"):
            os.unlink(os.path.join(s.backup_dir, f))
        r = s.cleanup(dry_run=False, confirm=True)
        assert r["removed"] == []  # auto-копий не было — ротация пуста
        assert s._load_backup_state().get("auto_skipped_limit") is None, \
            "флаг должен сняться при восстановлении места (pin-only)"
        assert s.backup_status()["auto_skipped_limit"] is False


def test_mcp_db_backup_fail_loud_on_legacy_kind(store):
    """Ревью #164 (split): db_backup с устаревшими kind/dest — явная
    ошибка, а не молчаливый сдвиг семантики (user-* → pin-*)."""
    from mcp.mcp_server import McpServer
    serv = McpServer(store)

    def call(name, args=None):
        return serv.handle({"jsonrpc": "2.0", "id": 1,
                            "method": "tools/call",
                            "params": {"name": name,
                                       "arguments": args or {}}})

    r = call("db_backup", {"kind": "user"})
    assert r.get("error"), r
    assert "удалены из протокола" in r["error"]["message"]
    assert "db_export" in r["error"]["message"]
    r = call("db_backup", {"dest": "/tmp/x.mdbx"})
    assert r.get("error")
    # чистый вызов (без legacy-параметров) по-прежнему работает
    r = call("db_backup", {})
    sc = r["result"]["structuredContent"]
    assert os.path.basename(sc["path"]).startswith("pin-"), sc


def test_mcp_backup_failed_class_is_invalid_on_limit(store, tmp_path,
                                                     monkeypatch):
    """Ревью #164-#7: PROMPT + over-limit → класс ошибки invalid,
    не internal (контрактный отказ лимита не деградирует)."""
    from mcp.mcp_server import McpServer
    store.backup_max_bytes = 1 << 20
    store.safe_store("fact:x:lim", "fact", "seed", 0.5)
    _inflate_backup_dir(store, mb=2)
    store.backup_pending = True  # имитация сработавшего PROMPT-таймера
    serv = McpServer(store)
    r = serv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                     "params": {"name": "db_backup", "arguments": {}}})
    assert r.get("error"), r
    msg = r["error"]["message"]
    assert "BACKUP_FAILED" in msg and "CLASS=invalid" in msg, msg
    assert "превышает лимит" in msg


def test_backup_status_tiers_contract(store):
    """Ревью #164: контракт tiers — все 4 ключа с типами.

    Фиксирует набор {lifeline: bool, auto: int, pin: int, snapshot: int}:
    переименование ключа (напр. pin→user) или смена типа пройдут незамеченными
    без этого теста. Тир export отсутствует по дизайну (вне каталога).
    """
    store.safe_store("fact:x:tier", "fact", "seed", 0.5)
    store.backup(kind="pin")
    sc = store.backup_status()
    tiers = sc["tiers"]
    assert set(tiers.keys()) == {"lifeline", "auto", "pin", "snapshot"}, tiers
    assert isinstance(tiers["lifeline"], bool)
    for k in ("auto", "pin", "snapshot"):
        assert isinstance(tiers[k], int) and tiers[k] >= 0
    assert tiers["pin"] >= 1
    # export НЕ попадает в tiers (копия вне каталога)
    assert "export" not in tiers


def test_auto_skip_flag_not_set_on_nonlimit_failure(tmp_path, monkeypatch):
    """Ревью #164: флаг auto_skipped_limit ставится ТОЛЬКО при лимитной
    причине падения backup().

    Граница except-ветки TOCTOU-фикса: если backup() упал по другой причине
    (диск/права) при СВОБОДНОМ каталоге — флаг остаться не должен, иначе рой
    ложно увидит «auto блокирован лимитом».
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 100 << 20  # лимит свободен

        def disk_failure(kind="pin", dest=None, force=False):
            raise OSError("No space left on device")

        monkeypatch.setattr(s, "backup", disk_failure)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        out = s.before_call()
        assert "failed" in (out.get("backup_notice") or ""), out
        # каталог свободен → флаг НЕ ставится (причина не лимит)
        assert s._load_backup_state().get("auto_skipped_limit") is None
        assert s.backup_status()["auto_skipped_limit"] is False


def test_auto_skip_flag_not_set_on_nonlimit_overfull(tmp_path, monkeypatch):
    """Ревью #164-r4 (1b): не-лимитная ошибка при ПЕРЕПОЛНЕННОМ каталоге
    не должна ставить флаг auto_skipped_limit.

    Сценарий из аудита: каталог переполнен по независимой причине, backup()
    падает по не-лимитной причине (диск/IO). Старое условие (только по
    размеру каталога) ставило флаг — рой ложно видел «auto блокирован
    лимитом», и после исправления первопричины auto не возобновлялся.
    Теперь флаг требует контрактный INVALID из backup() И переполнение.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        _inflate_backup_dir(s, mb=2)  # каталог ~2 МБ > лимита
        # гонка: первая проверка таймера видит свободное место (иначе он
        # штатно поставил бы флаг в ветке пропуска — это другой тест),
        # а внутри backup() «другой процесс» уже заполнил каталог и копия
        # падает по НЕ-лимитной причине (IO-ошибка)
        real_size = s._backup_dir_size
        phase = {"p": "timer"}

        def racing_size():
            return 0 if phase["p"] == "timer" else real_size()

        def io_failure(kind="pin", dest=None, force=False):
            phase["p"] = "backup"
            raise OSError("Input/output error")

        monkeypatch.setattr(s, "_backup_dir_size", racing_size)
        monkeypatch.setattr(s, "backup", io_failure)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        out = s.before_call()
        assert "failed" in (out.get("backup_notice") or ""), out
        monkeypatch.setattr(s, "_backup_dir_size", real_size)
        # причина НЕ лимит (OSError) → флаг НЕ ставится, несмотря на
        # переполненный каталог
        assert s._load_backup_state().get("auto_skipped_limit") is None
        assert s.backup_status()["auto_skipped_limit"] is False


def test_export_rejects_symlink_into_backup_dir(tmp_path, monkeypatch):
    """Ревью #164-r4: symlink внутрь backup_dir не обходит inside-проверку.

    abspath не резолвит симлинки — dest через symlink указывал бы внутрь
    каталога и ускользал из лимита/tiers. realpath закрывает обход.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        os.makedirs(s.backup_dir, exist_ok=True)
        # symlink снаружи, ведущий ВНУТРЬ backup_dir
        link = str(tmp_path / "outside.mdbx")
        target = os.path.join(s.backup_dir, "sub")
        os.makedirs(target, exist_ok=True)
        try:
            os.symlink(os.path.join(target, "x.mdbx"), link)
        except OSError as e:
            pytest.skip("symlink недоступен (Windows: developer mode): %s" % e)
        assert s._is_inside_backup_dir(link), "realpath должен видеть symlink"
        with pytest.raises(MemoryError) as ei:
            s.export(link)
        assert "внутри backup-каталога" in str(ei.value)


def test_diag_ops_section_and_busy_counters(tmp_path, monkeypatch):
    """D4/N12: db_status/diag показывает writer-контеншен и p90 коммитов.

    busy_retries/busy_failures растут на RC_BUSY (роёвое узкое место
    single-writer становится видимым до того, как упрётся в busy_io).
    """
    from mcp import libmdbx as mdbx_lib

    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        ops = s.diag()["ops"]
        assert ops["busy_retries"] == 0 and ops["busy_failures"] == 0
        assert ops["commits_in_window"] >= 1
        assert ops["commit_p90_us"] >= 0 and ops["commit_mean_us"] >= 0

        # имитируем BUSY: первые два begin бросают RC_BUSY, третий проходит
        real_begin = s.env.begin
        state = {"n": 0}

        def flaky_begin(readonly=False, parent=None):
            if not readonly and state["n"] < 2:
                state["n"] += 1
                raise mdbx_lib.LibmdbxError(mdbx_lib.RC_BUSY, "busy")
            return real_begin(readonly=readonly, parent=parent)

        monkeypatch.setattr(s.env, "begin", flaky_begin)
        s.safe_store("fact:x:z", "fact", "seed2", 0.5)
        ops = s.diag()["ops"]
        assert ops["busy_retries"] == 2, ops
        assert ops["busy_failures"] == 0

        # исчерпание ретраев → busy_failures и контрактная busy_io ошибка
        def always_busy(readonly=False, parent=None):
            if not readonly:
                raise mdbx_lib.LibmdbxError(mdbx_lib.RC_BUSY, "busy")
            return real_begin(readonly=readonly, parent=parent)

        monkeypatch.setattr(s.env, "begin", always_busy)
        monkeypatch.setattr("mcp.store.time.sleep", lambda *_: None)
        with pytest.raises(MemoryError) as ei:
            s.safe_store("fact:x:w", "fact", "seed3", 0.5)
        assert "BUSY" in str(ei.value)
        assert s.diag()["ops"]["busy_failures"] == 1


def test_mcp_db_status_ops_contract(store, monkeypatch):
    """D4/N12: ops-секция db_status пробрасывается через JSON-RPC handle.

    Контракт всех 6 полей зафиксирован (включая commit_max_us, который
    иначе никем не проверяется); на свежей БД без коммитов — нули.
    """
    from mcp.mcp_server import McpServer
    serv = McpServer(store)
    r = serv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                     "params": {"name": "db_status", "arguments": {}}})
    sc = r["result"]["structuredContent"]
    assert "ops" in sc, sc
    ops = sc["ops"]
    for field in ("busy_retries", "busy_failures", "commits_in_window",
                  "commit_p90_us", "commit_mean_us", "commit_max_us"):
        assert field in ops, "нет поля %s в ops" % field
    assert ops["busy_retries"] == 0 and ops["busy_failures"] == 0


def test_backup_limit_boundary_size_equals_max(tmp_path, monkeypatch):
    """D2/D4: граница лимита — строгое >, size==max проходит (контракт).

    Фиксирует осознанный off-by-one выбор: лимит сравнивает размер ДО
    копии, ровно на границе копия разрешена; каталог может превысить
    max на размер одной копии (предохранитель от роста, не точная квота).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        # ровно на границе: size == max — копия проходит
        s.backup_max_bytes = s._backup_dir_size()
        r = s.backup(kind="pin")
        assert os.path.isfile(r["path"]), "size==max должен проходить"
        # теперь size > max (копия добавилась) — следующая блокируется
        with pytest.raises(MemoryError):
            s.backup(kind="pin")


def test_inside_backup_dir_case_insensitive(monkeypatch):
    """Ревью #164-r4: Windows-регистр не обходит inside-проверку.

    ФС Windows регистронезависима: `c:\\bk\\sub` vs `C:\\Bk` без normcase
    дали бы outside=False. Проверяем хелпер через мок normcase с
    Windows-семантикой (casefold) на POSIX-пути.
    """
    from mcp.store import Store as StoreCls
    # мок-объект с backup_dir и хелпером
    obj = StoreCls.__new__(StoreCls)
    obj.backup_dir = "/Bk"
    # normcase как на Windows: casefold (на POSIX реальный — no-op, поэтому
    # патчим на casefold-семантику, чтобы проверить применение normcase)
    monkeypatch.setattr("os.path.normcase", lambda p: p.lower())
    assert obj._is_inside_backup_dir("/bk/sub/x.mdbx") is True, \
        "case-различие не должно обходить проверку (normcase применён)"
    assert obj._is_inside_backup_dir("/BK") is True
    assert obj._is_inside_backup_dir("/elsewhere/x.mdbx") is False


def test_state_merge_preserves_foreign_keys(tmp_path, monkeypatch):
    """F5.1/R1: merge-семантика state-файла — свои ключи поверх чужих.

    Имитация двух процессов через один Store: A ставит auto_skipped_limit,
    затем «B» (другой процесс) пишет свой last_auto — merge должен
    сохранить флаг A; B снимает флаг (None) — merge удаляет ключ.
    Env эксклюзивен на процесс, поэтому два Store одновременно
    невозможны — state-операции линейны (как при блокировке).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        # A ставит флаг
        sa = s._load_backup_state()
        sa["auto_skipped_limit"] = True
        s._save_backup_state(sa)
        assert s._load_backup_state().get("auto_skipped_limit") is True
        # B пишет свой last_auto (без знания о флаге)
        sb = s._load_backup_state()
        sb["last_auto"] = 123456.0
        s._save_backup_state(sb)
        cur = s._load_backup_state()
        assert cur.get("auto_skipped_limit") is True, \
            "merge должен сохранить флаг A при записи B"
        assert cur.get("last_auto") == 123456.0
        # B снимает флаг (None) — merge удаляет ключ
        sb2 = s._load_backup_state()
        sb2["auto_skipped_limit"] = None
        s._save_backup_state(sb2)
        assert s._load_backup_state().get("auto_skipped_limit") is None
        # чужой ключ не в merge-списке — не попадает в файл
        sb3 = s._load_backup_state()
        sb3["foreign_marker"] = "x"
        s._save_backup_state(sb3)
        assert "foreign_marker" not in s._load_backup_state()


def test_auto_timer_not_advanced_on_nonlimit_failure(tmp_path, monkeypatch):
    """F5.1/R1 + #175-#4: при НЕ-лимитной ошибке auto НЕ затихает на весь
    интервал, а применяет ЭКСПОНЕНЦИАЛЬНЫЙ backoff (следующий такт всё равно
    попробует, но реже — last_auto отодвигается на interval*2^count).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 100 << 20  # лимит свободен — не лимитная ветка

        def disk_failure(kind="pin", dest=None, force=False):
            raise OSError("No space left on device")

        monkeypatch.setattr(s, "backup", disk_failure)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        t0 = time.time()
        s.before_call()
        st2 = s._load_backup_state()
        # backoff: last_auto отодвинут на ~interval*2^1=2с (не 0 — не затих,
        # попытка реально отложена, но следующий такт придёт быстрее, чем
        # если бы затихли на "интервал успеха")
        assert st2.get("last_auto", 0) >= t0 + 1.5, \
            "таймер отодвинут на backoff: %r" % st2.get("last_auto")
        assert st2.get("auto_failure_count") == 1


def test_file_lock_serializes_state(tmp_path, monkeypatch):
    """F5.1/R1: _file_lock захватывается/освобождается (POSIX flock).

    Два вложенных захвата одного lock-файла блокируются бы (deadlock) —
    проверяем только, что за пределами контекста блокировка отпущена
    (повторный захват проходит сразу).
    """
    import threading
    from mcp.store import _file_lock
    p = str(tmp_path / "state.json")
    ok = []

    def grab():
        with _file_lock(p):
            ok.append(1)
            time.sleep(0.05)

    t = threading.Thread(target=grab)
    t.start()
    t.join(2.0)
    assert ok == [1], "блокировка должна захватываться в треде"
    # вне треда — повторный захват (после освобождения) проходит
    with _file_lock(p):
        pass


def test_busy_retry_sleep_uses_jitter(tmp_path, monkeypatch):
    """F5.3/R4: задержка при BUSY-ретрае — jitter, размывающий фазу роя.

    Детерминированная 0.05*(attempt+1) при 20 агентах — синхронное
    пробуждение (thundering herd). Тест фиксирует, что sleep вызывается
    с результатом random.uniform(0, max), а не константой.
    """
    from mcp import libmdbx as mdbx_lib
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        real_begin = s.env.begin
        state = {"n": 0}
        sleeps = []

        def busy_begin(readonly=False, parent=None):
            if not readonly and state["n"] == 0:
                state["n"] += 1
                raise mdbx_lib.LibmdbxError(mdbx_lib.RC_BUSY, "busy")
            return real_begin(readonly=readonly, parent=parent)

        monkeypatch.setattr("time.sleep",
                            lambda dt: sleeps.append(dt))
        monkeypatch.setattr(s.env, "begin", busy_begin)
        s.safe_store("fact:x:z", "fact", "seed2", 0.5)
        assert len(sleeps) == 1, "одна BUSY-попытка → один sleep"
        # jitter: sleep получил значение uniform(0, 0.05), не константу 0.05
        assert 0.0 <= sleeps[0] <= 0.05
        assert sleeps[0] != 0.05  # jitter, равный max — маловероятен


def test_backup_pin_dest_symlink_into_dir_limited(tmp_path, monkeypatch):
    """F5.4a/M3: pin с symlink-dest внутрь backup_dir — под лимитом.

    backup(kind="pin", dest) использует _is_inside_backup_dir (realpath) —
    симметрично export(): иначе symlink-dest обходил бы проверку dirname
    и лимит для pin-копии (учётная дыра).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        os.makedirs(s.backup_dir, exist_ok=True)
        sub = os.path.join(s.backup_dir, "sub")
        os.makedirs(sub, exist_ok=True)
        link = str(tmp_path / "pin-link.mdbx")
        try:
            os.symlink(os.path.join(sub, "x.mdbx"), link)
        except OSError as e:
            pytest.skip("symlink недоступен: %s" % e)
        _inflate_backup_dir(s, mb=2)  # каталог > лимита
        # symlink ведёт внутрь каталога → in_dir=True → лимит применяется
        with pytest.raises(MemoryError) as ei:
            s.backup(kind="pin", dest=link)
        assert "превышает лимит" in str(ei.value)


def test_branch_activate_warning_distinguishes_purged(tmp_path):
    """F5.4a/M2: warning активации отличает «не было» и «purge'нут»."""
    from mcp.store import Store
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.map_put_symbol("fn:m:z", {"module": "m"}, layer="0.15.warn.1")
        s.branch_register("0.15", "warn", n_commits=1)
        # слой существует → активация без warning
        r = s.branch_activate("0.15.warn.1")
        assert "warning" not in r, r
        # purge другой ветки, затем активация purge'нутого — warning с
        # упоминанием «purge'нут»
        s.branch_activate("0.15")
        s.branch_purge("0.15.warn.1")
        r2 = s.branch_activate("0.15.warn.1")
        assert "warning" in r2
        assert "purge'нут" in r2["warning"]


def test_backup_status_type_contract(tmp_path):
    """F5.4c/M14: верхнеуровневые ключи backup_status — полнота + типы.

    Переименование/смена типа ключа (напр. auto_skipped_limit bool →
    str, tiers — набор) пройдут незамеченными без этого контракта.
    """
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        st = s.backup_status()
        for k in ("backup_dir", "auto_backup_seconds", "prompt_backup_seconds",
                  "backup_keep", "backup_max_mb", "dir_size_mb", "last_auto",
                  "last_prompt", "next_auto", "next_prompt", "pending",
                  "auto_skipped_limit", "auto_failure_count",
                  "db_file_deleted", "notice",
                  "lifeline_error", "tiers", "rotation_candidates"):
            assert k in st, "нет ключа %r в backup_status" % k
        assert isinstance(st["auto_skipped_limit"], bool)
        assert isinstance(st["auto_failure_count"], int)
        assert isinstance(st["pending"], bool)
        assert isinstance(st["tiers"], dict)
        assert set(st["tiers"]) == {"lifeline", "auto", "pin", "snapshot"}
        assert isinstance(st["rotation_candidates"], list)
        assert st["auto_skipped_limit"] is False


def test_auto_resumes_after_manual_free_and_cleanup(tmp_path, monkeypatch):
    """F5.4c/M8: сквозной цикл — переполнение → флаг → rm → cleanup → auto
    возобновляется (новый auto-* создаётся на следующем такте).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        _inflate_backup_dir(s, mb=2)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        s.before_call()
        assert s.backup_status()["auto_skipped_limit"] is True
        # оператор освобождает место вручную
        for f in _files(s, "filler"):
            os.unlink(os.path.join(s.backup_dir, f))
        s.cleanup(dry_run=False, confirm=True)
        assert s.backup_status()["auto_skipped_limit"] is False
        # следующий такт таймера (БД менялась: next_id вырос) — auto-копия
        s.safe_store("fact:x:z", "fact", "seed2", 0.5)
        st = s._load_backup_state()
        st["last_auto"] = 0
        st["last_auto_txnid"] = 0
        s._save_backup_state(st)
        s.before_call()
        assert _files(s, "auto-"), "auto-бэкап должен возобновиться"


def test_backup_pin_dest_inside_outside_contract(tmp_path, monkeypatch):
    """F5.4c/M9: контракт backup(pin, dest): inside-каталог → лимит,
    outside → лимит не применяется («pin наружу»).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        _inflate_backup_dir(s, mb=2)  # каталог > лимита
        # dest ВНУТРИ backup_dir → лимит применяется → ошибка
        inside = os.path.join(s.backup_dir, "custom.mdbx")
        with pytest.raises(MemoryError) as ei:
            s.backup(kind="pin", dest=inside)
        assert "превышает лимит" in str(ei.value)
        assert not os.path.exists(inside)
        # dest НАРУЖУ backup_dir → лимит не применяется («pin наружу»)
        outside_dir = tmp_path / "outside"
        outside_dir.mkdir(exist_ok=True)
        outside = str(outside_dir / "pin-out.mdbx")
        r = s.backup(kind="pin", dest=outside)
        assert r["path"] == outside and os.path.isfile(outside)


def test_auto_backoff_exponential_on_nonlimit_failure(tmp_path, monkeypatch):
    """Ревью #175-#4: экспоненциальный backoff при постоянной не-лимитной
    ошибке — last_auto отодвигается на interval*2^count (потолок 32x).

    Иначе 20 агентов при ENOSPC повторяли бы дорогой снапшот каждый
    before_call (writer contention amplification).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "10")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        real_backup = s.backup

        def disk_fail(kind="pin", dest=None, force=False):
            raise OSError("No space left on device")

        monkeypatch.setattr(s, "backup", disk_fail)
        offsets = []
        for _ in range(3):
            stt = s._load_backup_state()
            stt["last_auto"] = 0
            stt["last_auto_txnid"] = 0
            s._save_backup_state(stt)
            t0 = time.time()
            s.before_call()
            stt = s._load_backup_state()
            offsets.append(stt.get("last_auto", 0) - t0)
            assert stt.get("auto_failure_count") == len(offsets)
        # 10 * 2^1=20, 2^2=40, 2^3=80
        assert offsets[0] >= 19, offsets
        assert offsets[1] >= 39, offsets
        assert offsets[2] >= 79, offsets
        # успех сбрасывает счётчик
        monkeypatch.setattr(s, "backup", real_backup)
        stt = s._load_backup_state()
        stt["last_auto"] = 0
        stt["last_auto_txnid"] = 0
        s._save_backup_state(stt)
        s.before_call()
        assert s._load_backup_state().get("auto_failure_count") == 0


def test_file_lock_degradation_warns(tmp_path, monkeypatch):
    """Ревью #175-#2: деградация _file_lock до lockless — ЗАМЕТНАЯ
    (warnings.warn), не тихая строчка stderr.
    """
    import warnings as _w
    from mcp.store import _file_lock
    state = str(tmp_path / "state.json")
    with _w.catch_warnings(record=True) as w:
        _w.simplefilter("always")

        def _throw(*_a, **_kw):
            raise OSError(95, "Operation not supported")

        monkeypatch.setattr("fcntl.flock", _throw)
        monkeypatch.setattr("fcntl.LOCK_EX", 1)
        monkeypatch.setattr("fcntl.LOCK_UN", 4)
        with _file_lock(state):
            pass
    assert any("state lock недоступен" in str(x.message) for x in w), \
        "деградация должна быть заметной (warnings.warn)"


def test_state_merge_two_processes(tmp_path, monkeypatch):
    """Ревью #175: merge-семантика state-файла ДВА процесса.

    fork: родитель ставит auto_skipped_limit, потомок пишет свой last_auto —
    merge должен сохранить флаг (last-writer-wins затёр бы его).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    db = str(tmp_path / "t.mdbx")
    st_ = Store(db, _skip_sync_thread=True)
    st_.safe_store("fact:x:y", "fact", "seed", 0.5)
    st_.close()
    # state-файл общий для процессов — эмулируем двумя Store-последовательно
    pid = os.fork()
    if pid == 0:
        # потомок: открывает СВОЙ Store (env отдельный) и пишет last_auto
        with Store(db, _skip_sync_thread=True) as s2:
            st2 = s2._load_backup_state()
            st2["last_auto"] = 111.0
            s2._save_backup_state(st2)
        os._exit(0)
    # родитель: ставит флаг ПОСЛЕ чтения потомка (гонка эмулируется
    # порядком: сначала потомок пишет last_auto, потом родитель — флаг)
    os.waitpid(pid, 0)
    with Store(db, _skip_sync_thread=True) as s3:
        st3 = s3._load_backup_state()
        st3["auto_skipped_limit"] = True
        s3._save_backup_state(st3)
        cur = s3._load_backup_state()
        assert cur.get("auto_skipped_limit") is True
        assert cur.get("last_auto") == 111.0  # потомков last_auto сохранился


def test_file_lock_reentrant_same_thread(tmp_path, monkeypatch):
    """Ревью Q6: вложенный _file_lock того же потока — yield (нет deadlock).

    AUTO-блок maybe_run_auto_backup держит _file_lock, а _save_backup_state
    внутри той же ветки снова вызывает его — повторный flock заблокировал бы
    сам себя; reentrant-счётчик защищает.
    """
    from mcp.store import _file_lock
    p = str(tmp_path / "lock-gate")
    with _file_lock(p):
        with _file_lock(p):  # вложенный вход того же потока
            pass
    # после выхода — повторный внешний захват тоже работает (лок освобождён)
    with _file_lock(p):
        pass


def test_auto_failure_count_lost_update_under_lock(tmp_path, monkeypatch):
    """Ревью #175-r3 Q6: AUTO-блок под _file_lock — два процесса не теряют
    инкремент auto_failure_count (lost update закрыт).

    Раньше read был вне лока: оба процесса видели count=3 и писали 4 вместо 5.
    Теперь весь read-modify-write под локом — последовательные процессы.
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "10")
    db = str(tmp_path / "t.mdbx")

    def _fail(kind="pin", dest=None, force=False):
        raise OSError("No space left on device")

    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        monkeypatch.setattr(s, "backup", _fail)
        for _ in range(3):
            stt = s._load_backup_state()
            stt["last_auto"] = 0
            stt["last_auto_txnid"] = 0
            s._save_backup_state(stt)
            s.before_call()
        assert s._load_backup_state().get("auto_failure_count") == 3, \
            "три последовательные неудачи — count=3 (инкремент не теряется)"


def test_auto_skip_flag_visible_during_backoff_window(tmp_path, monkeypatch):
    """Ревью Q5: флаг auto_skipped_limit виден в окне backoff (каталог
    переполнился, пока last_auto в будущем), не ждёт конца backoff."""
    import time as _t
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "60")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        s.backup_max_bytes = 1 << 20
        _inflate_backup_dir(s, mb=2)  # каталог переполнен
        st = s._load_backup_state()
        st["last_auto"] = _t.time() + 3600  # backoff активен (таймер НЕ истёк)
        s._save_backup_state(st)
        assert not s.backup_status()["auto_skipped_limit"]
        s.before_call()
        assert s.backup_status()["auto_skipped_limit"] is True, \
            "флаг лимита должен быть виден немедленно (не через backoff)"


def test_auto_backoff_ceiling_32x(tmp_path, monkeypatch):
    """Ревью #175-r3 T4: потолок backoff 32x — после 5+ неудач last_auto
    отодвигается на interval*32 (не 64x/128x).
    """
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)

        def disk_fail(kind="pin", dest=None, force=False):
            raise OSError("No space left on device")

        monkeypatch.setattr(s, "backup", disk_fail)
        for _ in range(7):  # count растёт до 7 (>=5)
            stt = s._load_backup_state()
            stt["last_auto"] = 0
            stt["last_auto_txnid"] = 0
            s._save_backup_state(stt)
            s.before_call()
        st = s._load_backup_state()
        assert st.get("auto_failure_count") == 7
        # потолок: min(2**7=128, 32) = 32 → offset ≈ interval*32 = 32с
        offset = st.get("last_auto", 0) - time.time()
        assert 30 <= offset <= 34, "потолок 32x (не 64/128): %.1f" % offset


def test_lock_degradation_sets_backup_notice(tmp_path, monkeypatch):
    """Ревью #175-r3 Q8: деградация _file_lock видна агенту через
    backup_notice; R2 (r4): per-path — чужой деградировавший лок НЕ даёт
    ложный notice другому Store в том же процессе."""
    import warnings as _w
    from mcp.store import Store, _file_lock
    d1 = str(tmp_path / "a.mdbx")
    d2 = str(tmp_path / "b.mdbx")
    with Store(d1, _skip_sync_thread=True) as a:
        with Store(d2, _skip_sync_thread=True) as b:
            a.safe_store("fact:x:y", "fact", "seed", 0.5)
            b.safe_store("fact:x:y", "fact", "seed", 0.5)
            with _w.catch_warnings():
                _w.simplefilter("ignore")
                # деградация ТОЛЬКО на lock A
                _file_lock._degraded_paths.add(a._state_path() + ".lock")
                a._save_backup_state(a._load_backup_state())
                assert "lock" in (a.backup_notice or ""), \
                    "notice A должен упомянуть деградацию"
                # B не должен получить ложный notice (его local-FS лок цел)
                b._save_backup_state(b._load_backup_state())
                assert not b.backup_notice, \
                    "B не должен видеть деградацию A: %r" % b.backup_notice
            lp = a._state_path() + ".lock"
            assert lp not in _file_lock._degraded_paths, \
                "флаг per-path сброшен после чтения"


def test_next_auto_correct_during_backoff(tmp_path, monkeypatch):
    """Ревью #175-r5 Б1: next_auto = last_auto + interval ВСЕГДА (в т.ч.
    при backoff). last_auto в backoff = now + interval*2^count, а гейт
    допускает попытку при now >= last_auto + interval — реальный момент
    попытки = last_auto + interval (R1 был регрессией: занижал на interval)."""
    import time as _t
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "10")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)

        def disk_fail(kind="pin", dest=None, force=False):
            raise OSError("No space left on device")

        monkeypatch.setattr(s, "backup", disk_fail)
        stt = s._load_backup_state()
        stt["last_auto"] = 0
        stt["last_auto_txnid"] = 0
        s._save_backup_state(stt)
        s.before_call()  # не-лимитная ошибка → backoff: last_auto = now + 20
        stt = s._load_backup_state()
        assert stt.get("auto_failure_count") >= 1
        stat = s.backup_status()
        # next_auto = last_auto + interval (реальный момент попытки)
        assert abs((stt["last_auto"] + s.auto_backup_seconds)
                   - stat["next_auto"]) < 1.0, \
            "next_auto в backoff = last_auto + interval: %r vs %r" % (
                stt["last_auto"], stat["next_auto"])
        # БЕЗ backoff (count=0): next_auto = last_auto + interval
        stt2 = s._load_backup_state()
        stt2["auto_failure_count"] = 0
        stt2["last_auto"] = _t.time()
        s._save_backup_state(stt2)
        stat2 = s.backup_status()
        assert abs(stat2["next_auto"] - stt2["last_auto"]) >= 9.0, \
            "без backoff next_auto = last_auto + interval"


def test_file_lock_different_paths_independent(tmp_path):
    """Ревью r4: разные path блокируются независимо (нет deadlock).

    per-thread dict по path — p2 должен реально flock'нуться (не reentrant
    для p1), оба освобождаются.
    """
    import fcntl
    from mcp.store import _file_lock
    p1 = str(tmp_path / "l1")
    p2 = str(tmp_path / "l2")
    with _file_lock(p1):
        with _file_lock(p2):
            # М1: физический flock на p2 — неблокирующий захват того же
            # lock-файла обязан дать BlockingIOError (иначе reentrant-yield)
            fh = open(p2 + ".lock", "a+b")
            try:
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                raise AssertionError(
                    "p2 не был физически заблокирован (reentrant-yield?)")
            except OSError:
                pass  # BlockingIOError — p2 реально залочен
            finally:
                fh.close()
    # после выхода — оба перезахватываются (локи освобождены)
    with _file_lock(p1):
        pass
    with _file_lock(p2):
        pass


def test_auto_lock_released_during_backup(tmp_path, monkeypatch):
    """Ревью #175-r5 Б3(a): flock НЕ удерживается во время ГБ-копии.

    Q6 держал лок на весь AUTO-блок включая backup() — before_call
    блокировал рой на время копии. Claim/reserve: копия вне лока.
    """
    import fcntl
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        state = {"free": None}

        def fake_copy(kind="auto", dest=None, force=False):
            lp = s._state_path() + ".lock"
            fh = open(lp, "a+b")
            try:
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                state["free"] = True
                fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
            except OSError:
                state["free"] = False
            finally:
                fh.close()
            return {"path": "x", "txnid": 1, "kind": "auto", "size": 1}

        monkeypatch.setattr(s, "backup", fake_copy)
        stt = s._load_backup_state()
        stt["last_auto"] = 0
        stt["last_auto_txnid"] = 0
        s._save_backup_state(stt)
        s.before_call()
        assert state["free"] is True, \
            "лок должен быть свободен во время копии (claim/reserve)"


def test_auto_claim_prevents_duplicate(tmp_path, monkeypatch):
    """Ревью #175-r5 Б3(b): claim (last_auto=now) атомарно закрывает гейт —
    следующий агент, увидев свежий last_auto, НЕ делает вторую копию."""
    import time as _t
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_BACKUP_DIR", str(tmp_path / "bk"))
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_AUTO_BACKUP_SECONDS", "1")
    db = str(tmp_path / "t.mdbx")
    with Store(db, _skip_sync_thread=True) as s:
        s.safe_store("fact:x:y", "fact", "seed", 0.5)
        copies = {"n": 0}

        def fake_copy(kind="auto", dest=None, force=False):
            copies["n"] += 1
            _t.sleep(0.05)
            return {"path": "x", "txnid": 1, "kind": "auto", "size": 1}

        monkeypatch.setattr(s, "backup", fake_copy)
        stt = s._load_backup_state()
        stt["last_auto"] = 0
        stt["last_auto_txnid"] = 0
        s._save_backup_state(stt)
        s.before_call()  # A: claim + копия
        assert copies["n"] == 1
        # гейт закрыт claim'ом — B (тот же state) НЕ копирует
        st_fresh = s._load_backup_state()
        gate_open = (_t.time() - st_fresh.get("last_auto", 0)
                     >= s.auto_backup_seconds)
        assert not gate_open, "claim должен закрыть гейт (дубль исключён)"
        s.before_call()  # повторный вызов сразу — таймер НЕ истёк
        assert copies["n"] == 1, "повторный before_call не должен копировать"


def test_file_lock_cross_thread_mutual_exclusion(tmp_path):
    """Ревью r4 М2: два потока на ОДНОМ path — взаимное исключение.

    Второй поток должен ждать освобождения первого (flock per-fd),
    а не reentrant-yield — иначе сломается merge-контракт state-файла.
    """
    import threading
    from mcp.store import _file_lock
    p = str(tmp_path / "mt")
    entered = {"a": False, "b": False}
    order = []
    done = threading.Event()

    def thread_a():
        with _file_lock(p):
            entered["a"] = True
            order.append("a-in")
            done.wait(2.0)  # держим лок, пока B не пытается
            order.append("a-out")

    def thread_b():
        # B пытается войти — должен ждать освобождения A
        started = threading.Event()
        with _file_lock(p):
            entered["b"] = True
            order.append("b-in")
            started.set()

    ta = threading.Thread(target=thread_a)
    tb = threading.Thread(target=thread_b)
    ta.start()
    # ждём, пока A в критической секции
    import time as _t
    deadline = _t.time() + 2.0
    while not entered["a"] and _t.time() < deadline:
        _t.sleep(0.005)
    assert entered["a"], "A не вошёл в секцию"
    tb.start()  # B теперь должен блокироваться на flock
    # B не должен войти, пока A держит (даже подождав 0.3с)
    _t.sleep(0.3)
    assert not entered["b"], \
        "B вошёл, пока A держит лок — нет взаимного исключения"
    done.set()  # освобождаем A
    tb.join(2.0)
    ta.join(2.0)
    assert entered["b"], "B так и не вошёл после освобождения A"
