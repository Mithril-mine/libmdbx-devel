#!/usr/bin/env python3
"""skynet run-bg-notify — асинхронный запуск команды с уведомлением на почту.

TASK-39: обёртка для фонового запуска произвольной команды (сборка, тесты,
скрипт) с опциональным `nice` и обязательным письмом координатору по
завершении/падении.

Обёртка запускается ДЕТАЧЕД (setsid + &) из run-bg-notify.sh; этот процесс
является монитором: ждёт завершения команды и шлёт письмо в inbox координатора.
Утилита не «умирает» вместе с оболочкой вызывающего.

Письмо:
  [bg-<job>-<launch_ts>|<ts>] run-bg-notify -> <inbox> : NOTIFY bg:done|bg:failed
    payload: команда, exit code, длительность, путь к логу (+ хвост при ошибке)

Идемпотентность: msg_id детерминирован от (job, launch_ts) — одно выполнение
ровно одно письмо. Журнал запусков: .skynet/bg-jobs.jsonl (append).

Usage:
  run-bg-notify.py --cmd "COMMAND" [--log FILE] [--nice N] [--inbox ENTITY] [--job ID]
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

BASE = "/sourcecraft/workspace/.skynet"
JOBS_FILE = os.path.join(BASE, "bg-jobs.jsonl")
DEFAULT_INBOX = "skynet_inbox_0.coordinator"
SERVER = os.environ.get("MEMORY_SERVER") or os.path.join(
    os.path.expanduser("~"), ".local", "share", "libmdbx-memory", "memory-mdbx-server.py")
MDBX_PATH = os.environ.get("MEMORY_MDBX_PATH") or os.path.join(
    os.path.expanduser("~"), ".local", "share", "libmdbx-memory", "graph.mdbx")
PIPE_DIR = os.path.join(BASE, "pipe")


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def mid(job, launch_ts):
    return "bg-%s-%s" % (job, launch_ts)


def journal(entry):
    """Append запись в bg-jobs.jsonl (не критично при сбое)."""
    try:
        with open(JOBS_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass


def _load_store():
    spec = importlib.util.spec_from_file_location("mmsrv", SERVER)
    srv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(srv)
    return srv.Store(MDBX_PATH)


def deliver(inbox, msg_id, ts, body, subject, payload):
    store = _load_store()
    store.create_entities([{"name": inbox, "entityType": "mailbox"}])
    letter = ("[%s|%s] run-bg-notify -> %s : NOTIFY %s | payload: %s | %s"
              % (msg_id, ts, inbox, subject, payload, body))
    res = store.add_observations([{"entityName": inbox, "contents": [letter]}])
    added = sum(len(r.get("addedObservations", [])) for r in res["results"])
    if added:
        signal(inbox)
    return added


def signal(inbox):
    slug = inbox.replace("skynet_inbox_", "")
    path = os.path.join(PIPE_DIR, "%s.fifo" % slug)
    if not os.path.exists(path):
        return
    try:
        fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, b"wake\n")
        os.close(fd)
    except OSError:
        pass


def tail_log(path, n=15):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 4096))
            return f.read().decode("utf-8", "replace").strip().splitlines()[-n:]
    except OSError:
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--log")
    ap.add_argument("--nice", type=int, default=5)
    ap.add_argument("--inbox", default=DEFAULT_INBOX)
    ap.add_argument("--job", default=None)
    args = ap.parse_args()

    launch_ts = now_iso()
    job = args.job or "bg-%s-%d" % (datetime.now().strftime("%H%M%S"), os.getpid())
    log_path = args.log or os.path.join(BASE, "tmp", "run-bg", job + ".log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)

    nice_prefix = ["nice", "-n", str(args.nice)] if args.nice else []
    cmdline = (nice_prefix + ["/bin/bash", "-c", args.cmd]) if nice_prefix \
        else ["/bin/bash", "-c", args.cmd]

    start = time.monotonic()
    try:
        with open(log_path, "a", encoding="utf-8", errors="replace") as lf:
            lf.write("\n[%s] run-bg-notify job=%s: %s\n" % (launch_ts, job, args.cmd))
            lf.flush()
            p = subprocess.run(cmdline, stdout=lf, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL)
        rc = p.returncode
    except OSError as e:
        rc = -1
        try:
            with open(log_path, "a", encoding="utf-8") as lf:
                lf.write("[%s] spawn failed: %s\n" % (now_iso(), e))
        except OSError:
            pass
    duration = time.monotonic() - start
    ts = now_iso()

    subject = "bg:done" if rc == 0 else "bg:failed"
    payload = "rc=%s duration=%.1fs log=%s" % (rc, duration, log_path)
    body = "cmd: %s" % args.cmd
    if rc != 0:
        t = tail_log(log_path)
        if t:
            body += "; tail: " + " | ".join(line.strip()[:80] for line in t[-3:])

    added = deliver(args.inbox, mid(job, launch_ts), ts, body, subject, payload)
    journal({
        "job": job, "launch_ts": launch_ts, "finish_ts": ts,
        "cmd": args.cmd, "rc": rc, "duration_s": round(duration, 2),
        "log": log_path, "inbox": args.inbox, "delivered": added,
    })
    sys.exit(0 if rc == 0 else 1)


if __name__ == "__main__":
    main()