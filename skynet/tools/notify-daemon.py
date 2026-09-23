#!/usr/bin/env python3
"""skynet notify-daemon — GitHub + SourceCraft события → почта координатора.

TASK-38: доставка событий от пользователей (issues, комментарии, PR, релизы)
и статусов CI в `skynet_inbox_0.coordinator` (почта координатора).

Принципы (по заданию):
  * оперативность  — поллинг раз в ~60с;
  * робустность    — недоступность источника НЕ двигает watermark, retry без потерь;
  * идемпотентность — строго монотонный watermark, дедуп по event-id,
    письма-маркеры `[notify-<src>-<type>-<eventid>|<ts>]` (детерминированный
    msg_id от event-id: переживает рестарт/падение между доставкой и save_state),
    watermark обновляется ТОЛЬКО после успешной доставки.

Writer-путь: библиотека Store из memory-mdbx-server.py (graph.mdbx, ACID),
MCP-сессия НЕ дублируется.

Канонический источник: репозиторий skynet/tools/notify-daemon.py (ветка
feature/task38-notify-daemon); в .skynet/tools/ кладётся рабочая копия.

Usage:
  notify-daemon.py [--once] [--interval 60] [--dry-run] [--debug]
    --once       один цикл поллинга и выход (для cron/тестов)
    --dry-run    печатать события, не писать в память (watermark не трогать)
    --interval N период поллинга (сек), по умолчанию 60
"""
import argparse
import fcntl
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone

BASE = "/sourcecraft/workspace/.skynet"
STATE_FILE = os.path.join(BASE, "notify-state.json")
LOG_FILE = os.path.join(BASE, "notify.log")
GITHUB_REPO = os.environ.get("GITHUB_REPO", "Mithril-mine/libmdbx-devel")
SOURCE_REPO = os.environ.get("SOURCE_REPO", "dqdkfa/libmdbx-devel")
TARGET_INBOX = os.environ.get("NOTIFY_INBOX", "skynet_inbox_0.coordinator")
SENDER = "notify-daemon"
SERVER = os.environ.get("MEMORY_SERVER") or os.path.join(
    os.path.expanduser("~"), ".local", "share", "libmdbx-memory", "memory-mdbx-server.py")
MDBX_PATH = os.environ.get("MEMORY_MDBX_PATH") or os.path.join(
    os.path.expanduser("~"), ".local", "share", "libmdbx-memory", "graph.mdbx")
PIPE_DIR = os.path.join(BASE, "pipe")
MAX_PAGES = 5
BOOTSTRAP_HOURS = 24   # на первом прогоне свежего потока доставляем события этого окна

ISSUE_EVENTS_OF_INTEREST = {
    "opened", "closed", "reopened", "referenced", "cross-referenced",
    "labeled", "assigned", "unassigned", "milestoned",
}
RUN_TERMINAL = {"success", "failure", "cancelled", "timed_out", "action_required"}


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def in_boot_window(ts):
    """True, если ts (ISO) попадает в bootstrap-окно [now-BOOTSTRAP_HOURS, now]."""
    if not ts:
        return False
    try:
        t = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except ValueError:
        return False
    cutoff = datetime.now(timezone.utc) - timedelta(hours=BOOTSTRAP_HOURS)
    return cutoff <= t <= datetime.now(timezone.utc)


def log(msg):
    line = "[%s] %s" % (now_iso(), msg)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass
    if os.environ.get("NOTIFY_DEBUG"):
        print(line, file=sys.stderr)


def run(cmd, timeout=40):
    """subprocess wrapper; returns (rc, stdout, stderr)."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout %ss" % timeout
    except OSError as e:
        return -1, "", str(e)


def gh_json(args, pages=False):
    """gh api GET с опциональной пагинацией до MAX_PAGES.

    При ЛЮБОЙ ошибке страницы возвращает None — частичные данные НЕ
    используются, watermark не двигается (источник «недоступен»).
    """
    out_all = []
    page_args = list(args)
    per_page = None
    for a in page_args:
        if a == "per_page" or (isinstance(a, str) and a.startswith("per_page=")):
            per_page = a.split("=")[-1] if "=" in a else None
    for _ in range(MAX_PAGES if pages else 1):
        rc, out, err = run(["gh", "api", "--method", "GET"] + page_args)
        if rc != 0:
            log("gh api %s failed rc=%s err=%s" % (page_args[0], rc, err[:200]))
            return None
        try:
            batch = json.loads(out)
        except json.JSONDecodeError:
            log("gh api %s: bad json" % page_args[0])
            return None
        out_all += batch if isinstance(batch, list) else ([batch] if batch else [])
        if not pages or not isinstance(batch, list) or len(batch) == 0:
            break
        if per_page and len(batch) < int(per_page):
            break                       # последняя страница
        m = re.search(r"page=(\d+)", page_args[-1] if page_args else "")
        cur = int(m.group(1)) if m else 1
        if cur >= MAX_PAGES:
            break
        # чистая постраничная выборка: -f page=N (gh api добавляет в query)
        page_args = [a for a in page_args if not re.match(r"^page=\d+$", a)]
        page_args.append("page=%d" % (cur + 1))
    return out_all


# ------------------------------------------------------------- состояние --

def load_state():
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(state):
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False, indent=1)
    os.replace(tmp, STATE_FILE)


# ------------------------------------------------------------ доставка --

def _load_store():
    spec = importlib.util.spec_from_file_location("mmsrv", SERVER)
    srv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(srv)
    return srv.Store(MDBX_PATH)


def deliver(events, dry_run=False):
    """Append letters to TARGET_INBOX. Returns delivered event-ids."""
    if not events:
        return []
    if dry_run:
        log("DRY-RUN: %d событий (не пишу)" % len(events))
        for e in events:
            print("  [%s] %s %s" % (e["mid"], e["type"], e["subject"]))
        return [e["id"] for e in events]

    store = _load_store()
    # target entity may be absent in graph (rename history) — ensure exists.
    store.create_entities([{"name": TARGET_INBOX, "entityType": "mailbox"}])
    obs_list = []
    for e in events:
        letter = ("[%s|%s] %s -> %s : NOTIFY %s | payload: %s | %s"
                  % (e["mid"], e["ts"], SENDER, TARGET_INBOX,
                     e["type"], e["payload"], e["body"]))
        obs_list.append({"entityName": TARGET_INBOX, "contents": [letter]})
    res = store.add_observations(obs_list)
    added = sum(len(r.get("addedObservations", [])) for r in res["results"])
    if added != len(events):
        raise RuntimeError(
            "add_observations добавил %d/%d — доставка неполная, watermark НЕ тронут"
            % (added, len(events)))
    log("delivered %d/%d наблюдений в %s" % (added, len(events), TARGET_INBOX))
    signal_coordinator()
    return [e["id"] for e in events]


def signal_coordinator():
    path = os.path.join(PIPE_DIR, "0.coordinator.fifo")
    if not os.path.exists(path):
        return
    try:
        fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, b"wake\n")
        os.close(fd)
    except OSError:
        pass


def mid(src, typ, eid):
    """Детерминированный msg_id от event-id (идемпотентность через рестарты)."""
    return "notify-%s-%s-%s" % (src, typ, eid)


# ------------------------------------------------------------- источники --

def poll_github_issues(state):
    """Issue-события через /issues/events (полный поток, с пагинацией)."""
    key = "gh-issues"
    fresh = key not in state
    wm = state.setdefault(key, {"last_id": 0})
    data = gh_json(["repos/%s/issues/events" % GITHUB_REPO,
                    "-f", "per_page=100"], pages=True)
    if data is None:
        return []          # источник недоступен — watermark не двигаем
    evs = []
    for ev in data:
        eid = ev.get("id")
        if not eid or eid <= wm["last_id"]:
            continue
        typ = ev.get("event", "")
        if typ not in ISSUE_EVENTS_OF_INTEREST:
            continue
        issue = ev.get("issue") or {}
        actor = (ev.get("actor") or {}).get("login", "?")
        n = issue.get("number", "?")
        title = (issue.get("title") or "").strip()[:90]
        url = issue.get("html_url") or ("https://github.com/%s/issues/%s"
                                        % (GITHUB_REPO, n))
        if typ == "cross-referenced":
            subj = "issue #%s referenced" % n
        else:
            subj = "issue #%s %s by %s" % (n, typ, actor)
        evs.append({
            "id": eid, "ts": ev.get("created_at") or now_iso(),
            "type": "issue:" + typ, "subject": subj,
            "payload": url, "body": "%s — %s" % (title, subj),
            "mid": mid("gh", typ, eid),
        })
    if evs:
        if fresh:
            wm["last_id"] = max(e["id"] for e in evs)
            kept = [e for e in evs if in_boot_window(e["ts"])]
            log("%s: bootstrap окно %dh — доставляю %d, baseline last_id=%d"
                % (key, BOOTSTRAP_HOURS, len(kept), wm["last_id"]))
            return kept
        wm["last_id"] = max(e["id"] for e in evs)
    return evs


def poll_github_pulls(state):
    """PR: открытие/закрытие/мерж/обновление (через /pulls sorted by updated)."""
    key = "gh-pulls"
    fresh = key not in state
    wm = state.setdefault(key, {"known": {}})
    known = wm["known"]
    data = gh_json(["repos/%s/pulls" % GITHUB_REPO,
                    "-f", "state=all", "-f", "sort=updated",
                    "-f", "direction=desc", "-f", "per_page=50"], pages=True)
    if data is None:
        return []
    evs = []
    for pr in data:
        n = pr.get("number")
        if not n:
            continue
        cur = {"state": pr.get("state"),
               "merged": bool(pr.get("merged_at")),
               "updated": pr.get("updated_at") or ""}
        prev = known.get(str(n))
        if prev is None:
            if fresh:
                if in_boot_window(cur["updated"]):
                    typ = ("pr:merged" if cur["merged"]
                           else "pr:closed" if cur["state"] == "closed"
                           else "pr:opened")
                    subj = ("PR #%s merged" % n if typ == "pr:merged"
                            else "PR #%s closed" % n if typ == "pr:closed"
                            else "PR #%s opened" % n)
                    title = (pr.get("title") or "").strip()[:90]
                    author = (pr.get("user") or {}).get("login", "?")
                    url = pr.get("html_url") or ("https://github.com/%s/pull/%s"
                                                 % (GITHUB_REPO, n))
                    evs.append({
                        "id": "%s@%s" % (n, cur["updated"][:19]),
                        "ts": cur["updated"] or now_iso(), "type": typ,
                        "subject": subj, "payload": url,
                        "body": "%s — PR by %s" % (title, author),
                        "mid": mid("gh", typ.replace(":", "-"),
                                   "%s@%s" % (n, cur["updated"][:19])),
                    })
                known[str(n)] = cur
                continue
            if cur["merged"]:
                typ, subj = "pr:merged", "PR #%s merged" % n
            elif cur["state"] == "closed":
                typ, subj = "pr:closed", "PR #%s closed" % n
            else:
                typ, subj = "pr:opened", "PR #%s opened" % n
        elif prev["state"] != cur["state"] or prev["merged"] != cur["merged"]:
            if cur["merged"]:
                typ, subj = "pr:merged", "PR #%s merged" % n
            elif cur["state"] == "closed":
                typ, subj = "pr:closed", "PR #%s closed" % n
            else:
                typ, subj = "pr:reopened", "PR #%s reopened" % n
        else:
            known[str(n)] = cur
            continue
        title = (pr.get("title") or "").strip()[:90]
        author = (pr.get("user") or {}).get("login", "?")
        url = pr.get("html_url") or ("https://github.com/%s/pull/%s"
                                     % (GITHUB_REPO, n))
        eid = "%s@%s" % (n, cur["updated"][:19])
        evs.append({
            "id": eid, "ts": cur["updated"] or now_iso(), "type": typ,
            "subject": subj, "payload": url,
            "body": "%s — PR by %s" % (title, author),
            "mid": mid("gh", typ.replace(":", "-"), eid),
        })
        known[str(n)] = cur
    return evs


def poll_github_comments(state):
    """Комментарии к issues/PR (sort=created desc, с пагинацией)."""
    key = "gh-comments"
    fresh = key not in state
    wm = state.setdefault(key, {"last_id": 0})
    data = gh_json(["repos/%s/issues/comments" % GITHUB_REPO,
                    "-f", "sort=created", "-f", "direction=desc",
                    "-f", "per_page=50"], pages=True)
    if data is None:
        return []
    evs = []
    for c in data:
        cid = c.get("id")
        if not cid or cid <= wm["last_id"]:
            continue
        iu = c.get("issue_url") or ""
        m = re.search(r"/issues/(\d+)$", iu)
        n = m.group(1) if m else "?"
        author = (c.get("user") or {}).get("login", "?")
        body = (c.get("body") or "").replace("\n", " ").strip()[:120]
        url = c.get("html_url") or ("https://github.com/%s/issues/%s#comment-%s"
                                    % (GITHUB_REPO, n, cid))
        evs.append({
            "id": cid, "ts": c.get("created_at") or now_iso(),
            "type": "issue:commented",
            "subject": "issue #%s commented by %s" % (n, author),
            "payload": url, "body": body,
            "mid": mid("gh", "commented", cid),
        })
    if evs:
        if fresh:
            wm["last_id"] = max(e["id"] for e in evs)
            kept = [e for e in evs if in_boot_window(e["ts"])]
            log("%s: bootstrap окно %dh — доставляю %d, baseline last_id=%d"
                % (key, BOOTSTRAP_HOURS, len(kept), wm["last_id"]))
            return kept
        wm["last_id"] = max(e["id"] for e in evs)
    return evs


def poll_github_runs(state):
    """CI runs (все ветки): статус queued/in_progress → completed(conclusion)."""
    key = "gh-runs"
    wm = state.setdefault(key, {"known": {}})
    known = wm["known"]
    rc, out, err = run(["gh", "run", "list", "-R", GITHUB_REPO,
                        "--limit", "30",
                        "--json", "databaseId,name,status,conclusion,headSha,createdAt"])
    if rc != 0:
        log("gh run list failed: %s" % err[:200])
        return []
    try:
        runs = json.loads(out)
    except json.JSONDecodeError:
        return []
    evs = []
    for r in runs:
        rid = r.get("databaseId")
        if not rid:
            continue
        cur = {"status": r.get("status"), "conclusion": r.get("conclusion")}
        prev = known.get(str(rid))
        known[str(rid)] = cur
        if prev is None:
            # новые запуски и bootstrap-бэкап не шумим: статусы CI уже видны
            # в digest оркестратора (секция GitHub CI); письма — только для
            # переходов queued/in_progress -> completed в живом цикле.
            continue
        if (prev["status"] != "completed"
                and cur["status"] == "completed"):
            concl = cur["conclusion"] or "?"
            flag = "ok" if concl == "success" else "RED"
            name = r.get("name", "?")
            sha = (r.get("headSha") or "")[:8]
            url = ("https://github.com/%s/actions/runs/%s"
                   % (GITHUB_REPO, rid))
            evs.append({
                "id": rid, "ts": now_iso(), "type": "ci:" + (concl or "done"),
                "subject": "CI %s %s (%s) %s" % (name, flag, concl, sha),
                "payload": url, "body": "%s @%s -> %s" % (name, sha, concl),
                "mid": mid("gh", "run-" + (concl or "done"), rid),
            })
    return evs


def poll_github_releases(state):
    key = "gh-releases"
    fresh = key not in state
    wm = state.setdefault(key, {"last_id": 0})
    data = gh_json(["repos/%s/releases" % GITHUB_REPO, "-f", "per_page=10"])
    if data is None:
        return []
    evs = []
    for rel in data:
        rid = rel.get("id")
        if not rid or rid <= wm["last_id"]:
            continue
        tag = rel.get("tag_name", "?")
        name = (rel.get("name") or tag)[:90]
        url = rel.get("html_url") or ("https://github.com/%s/releases/tag/%s"
                                      % (GITHUB_REPO, tag))
        evs.append({
            "id": rid, "ts": rel.get("published_at") or now_iso(),
            "type": "release", "subject": "release %s published" % tag,
            "payload": url, "body": name,
            "mid": mid("gh", "release", rid),
        })
    if evs:
        if fresh:
            wm["last_id"] = max(e["id"] for e in evs)
            kept = [e for e in evs if in_boot_window(e["ts"])]
            log("%s: bootstrap окно %dh — доставляю %d, baseline last_id=%d"
                % (key, BOOTSTRAP_HOURS, len(kept), wm["last_id"]))
            return kept
        wm["last_id"] = max(e["id"] for e in evs)
    return evs


def poll_sourcecraft(state):
    """SourceCraft issues/PRs (best-effort; API может быть пуст/недоступен)."""
    evs = []
    for kind in ("issue", "pr"):
        key = "sc-" + kind + "s"
        fresh = key not in state
        wm = state.setdefault(key, {"last_id": 0})
        cmd = ["src", kind, "list", "-R", SOURCE_REPO,
               "--limit", "30", "--json-compact"]
        rc, out, err = run(cmd, timeout=40)
        if rc != 0:
            log("src %s list failed rc=%s: %s" % (kind, rc, err[:150]))
            continue
        if not out or out.strip() == "null" or out.strip() == "":
            continue
        try:
            items = json.loads(out)
        except json.JSONDecodeError:
            log("src %s list: bad json" % kind)
            continue
        for it in items or []:
            eid = it.get("id") or it.get("number")
            if not eid or int(eid) <= wm["last_id"]:
                continue
            title = (it.get("title") or "")[:90]
            url = it.get("url") or ("https://sourcecraft.dev/%s/%s/%s/%s"
                                    % (SOURCE_REPO.split("/")[0], kind, kind, eid))
            st = it.get("status") or it.get("state") or "?"
            evs.append({
                "id": int(eid), "ts": now_iso(),
                "type": kind + ":" + str(st),
                "subject": "SourceCraft %s #%s (%s)" % (kind, eid, st),
                "payload": url, "body": title,
                "mid": mid("sc", kind + "-" + str(st), eid),
            })
        if evs:
            if fresh:
                wm["last_id"] = max(e["id"] for e in evs)
                kept = [e for e in evs if in_boot_window(e["ts"])]
                log("%s: bootstrap окно %dh — доставляю %d, baseline last_id=%d"
                    % (key, BOOTSTRAP_HOURS, len(kept), wm["last_id"]))
                evs = kept
            else:
                # ВАЖНО: watermark двигается и на не-fresh циклах, иначе каждый
                # элемент ре-доставляется каждые ~60с (дубли в почту).
                wm["last_id"] = max(e["id"] for e in evs)
    return evs


# ----------------------------------------------------------------- цикл --

def one_pass(state, dry_run):
    log("poll start (dry=%s)" % dry_run)
    events = []
    events += poll_github_issues(state)
    events += poll_github_pulls(state)
    events += poll_github_comments(state)
    events += poll_github_runs(state)
    events += poll_github_releases(state)
    events += poll_sourcecraft(state)

    # дедуп по mid (в т.ч. внутри одной волны)
    seen, uniq = set(), []
    for e in events:
        if e["mid"] in seen:
            continue
        seen.add(e["mid"])
        uniq.append(e)
    if not uniq:
        log("no new events")
        return 0
    try:
        delivered = deliver(uniq, dry_run=dry_run)
    except Exception as ex:      # noqa: BLE001 — доставка упала: не двигаем watermark
        log("delivery FAILED: %s — watermark НЕ тронут, повторю на след. цикле" % ex)
        return 1
    if not dry_run:
        save_state(state)
        log("watermark updated, %d событий доставлено" % len(delivered))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    if args.debug:
        os.environ["NOTIFY_DEBUG"] = "1"

    # Single-instance: эксклюзивный flock на время жизни процесса (не TOCTOU,
    # в отличие от pidfile). Два демона = дубли писем (см. REV:cmake MAJOR2).
    lock_fd = None
    if not args.dry_run:
        lock_fd = os.open(STATE_FILE + ".lock", os.O_CREAT | os.O_RDWR, 0o644)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            log("another notify-daemon instance holds %s.lock — exit" % STATE_FILE)
            os.close(lock_fd)
            return 1

    try:
        state = load_state()
        if args.dry_run:
            rc = one_pass(state, dry_run=True)
            return rc
        while True:
            try:
                one_pass(state, dry_run=False)
            except Exception as ex:      # noqa: BLE001 — daemon не должен падать
                log("cycle error: %s" % ex)
            if args.once:
                return 0
            time.sleep(max(10, args.interval))
    finally:
        if lock_fd is not None:
            os.close(lock_fd)            # flock снимается автоматически


if __name__ == "__main__":
    sys.exit(main())