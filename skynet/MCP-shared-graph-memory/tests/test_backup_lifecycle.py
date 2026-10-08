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
from mcp.libmdbx import LibmdbxError
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


def test_backup_user_compact_snapshot(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        s.vocab_add("practice", "durability-aa")
        s.safe_store("fact:practice:durability-aa", "fact", "bk", 0.5)
        r = s.backup(kind="user")
        assert r["kind"] == "user"
        assert os.path.isfile(r["path"])
        assert os.path.basename(r["path"]).startswith("user-")
        assert r["txnid"] > 0
        assert _files(s, "user-") == [os.path.basename(r["path"])]
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
        assert _files(s, "user-")
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
        assert _files(s, "user-") == []
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
        # тир user не в кандидатах
        s.backup(kind="user")
        assert _files(s, "user-")
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
    r = call("db_backup", {"kind": "user"})
    sc = r["result"]["structuredContent"]
    assert sc["approved"] is True and sc.get("path"), sc
    r = call("db_backup_status")
    assert "tiers" in r["result"]["structuredContent"]
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
        time.sleep(1.2)
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    time.sleep(0.3)
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