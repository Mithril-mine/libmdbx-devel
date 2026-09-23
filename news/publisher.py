#!/usr/bin/env python3
"""skynet-news publisher — Telegram Bot API bridge (TASK-32).

Publishes draft posts from news/drafts/*.md to a Telegram chat via
https://api.telegram.org/bot<TOKEN>/sendMessage (plain text).

Usage:
  python3 news/publisher.py [--dry-run] [--draft ID] [--chat-id @id]
    --dry-run   validate and print, do not send (default when TELEGRAM_BOT_TOKEN unset)
    --draft ID  process only the given draft id (default: all status:ready)
    --chat-id   override target chat id/username

Exit codes:
  0  all ready drafts published (or would be, in dry-run)
  1  validation or delivery failure (no post left half-published)
  2  usage error

Design (see skynet/DESIGN.md of task):
  - queue = git: a draft with status:ready is pending; on success the script
    rewrites frontmatter to status:posted + posted_at, the workflow commits it.
  - idempotency: status:posted is skipped; a failed send keeps status:ready so
    the next run retries.
  - plain text parse_mode (no HTML/Markdown escaping risks; posts <=400 chars).
"""

import argparse
import datetime
import json
import os
import re
import sys
import urllib.error
import urllib.request

DRAFTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "drafts")
API = "https://api.telegram.org/bot{token}/sendMessage"
MAX_TEXT = 400
MAX_TITLE = 60
ALLOWED_CTA = (
    "Поделитесь мнением о новой фиче",
    "Поставьте реакцию, если тема полезна",
    "Предложите идею для следующего улучшения",
    "Посмотрите PR по ссылке",
)


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_frontmatter(text):
    """Minimal YAML-subset parser for the --- frontmatter block."""
    m = re.match(r"^---\s*\n(.*?)\n---\s*\n?(.*)$", text, re.S)
    if not m:
        return {}, text
    meta, body = {}, m.group(2)
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, _, val = line.partition(":")
        if key.strip():
            meta[key.strip()] = val.strip().strip('"').strip("'")
    return meta, body


def validate(meta, body):
    """Return list of problems (empty = ready to publish)."""
    problems = []
    text = body.strip()
    if len(text) > MAX_TEXT:
        problems.append("длина поста %d > %d символов" % (len(text), MAX_TEXT))
    title = meta.get("title", "")
    if len(title) > MAX_TITLE:
        problems.append("заголовок %d > %d символов" % (len(title), MAX_TITLE))
    cta = meta.get("cta", "")
    if not cta:
        problems.append("нет cta (обязателен)")
    elif cta not in ALLOWED_CTA:
        problems.append("cta не из списка codex §2: %r" % cta)
    if not meta.get("id"):
        problems.append("нет id в frontmatter")
    if meta.get("status") != "ready":
        problems.append("status != ready: %r" % meta.get("status"))
    if not meta.get("target"):
        problems.append("нет target (test|@libmdbx)")
    if not text:
        problems.append("пустое тело поста")
    return problems


def load_drafts(draft_filter=None):
    out = []
    if not os.path.isdir(DRAFTS_DIR):
        return out
    for name in sorted(os.listdir(DRAFTS_DIR)):
        if not name.endswith(".md"):
            continue
        path = os.path.join(DRAFTS_DIR, name)
        with open(path, encoding="utf-8") as f:
            raw = f.read()
        meta, body = parse_frontmatter(raw)
        if not meta.get("id"):
            continue  # не драфт (README и т.п.)
        if draft_filter and meta.get("id") != draft_filter:
            continue
        if not draft_filter and meta.get("status") != "ready":
            print("[%s] skip: status=%r (публикуются только ready)" % (meta["id"], meta.get("status")))
            continue
        out.append({"path": path, "name": name, "meta": meta, "body": body})
    return out


def send_message(token, chat_id, text):
    payload = json.dumps({
        "chat_id": chat_id,
        "text": text,
        "disable_web_page_preview": True,
    }).encode()
    req = urllib.request.Request(API.format(token=token), data=payload,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        raise RuntimeError("HTTP %d: %s" % (e.code, detail[:300])) from e
    except urllib.error.URLError as e:
        raise RuntimeError("сеть недоступна: %s" % e.reason) from e


def mark_posted(path, ts):
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    raw = re.sub(r"(?m)^status:.*$", "status: posted", raw, count=1)
    raw = re.sub(r"(?m)^posted_at:.*$", "posted_at: %s" % ts, raw, count=1)
    if "posted_at:" not in raw:
        raw = re.sub(r"(?m)^(status: posted)$", r"\1\nposted_at: %s" % ts, raw, count=1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(raw)


def main():
    ap = argparse.ArgumentParser(description="skynet-news Telegram publisher")
    ap.add_argument("--dry-run", action="store_true",
                    help="validate and print, do not send")
    ap.add_argument("--draft", help="publish only this draft id")
    ap.add_argument("--chat-id", help="override target chat id/username")
    args = ap.parse_args()

    token = os.environ.get("TELEGRAM_BOT_TOKEN", "")
    dry = args.dry_run or not token
    if args.dry_run:
        print("== dry-run: отправка отключена ==")

    drafts = load_drafts(args.draft)
    if not drafts:
        print("no ready/pending drafts in %s" % DRAFTS_DIR)
        return 0

    rc = 0
    for d in drafts:
        mid = d["meta"].get("id", d["name"])
        problems = validate(d["meta"], d["body"])
        if problems:
            print("[%s] VALIDATION FAIL: %s" % (mid, "; ".join(problems)))
            rc = 1
            continue
        chat = args.chat_id or d["meta"].get("target")
        print("---- [%s] -> %s ----" % (mid, chat))
        print(d["body"].strip())
        print("-----------------------")
        if dry:
            print("[%s] OK (dry-run, не отправлено)" % mid)
            continue
        try:
            resp = send_message(token, chat, d["body"].strip())
        except RuntimeError as e:
            print("[%s] SEND FAIL: %s (остаётся status:ready — догонит след. прогон)" % (mid, e))
            rc = 1
            continue
        if not (resp or {}).get("ok"):
            print("[%s] SEND REJECTED: %s" % (mid, json.dumps(resp)[:300]))
            rc = 1
            continue
        ts = now_iso()
        mark_posted(d["path"], ts)
        print("[%s] PUBLISHED (%s), помечен status:posted" % (mid, ts))

    return rc


if __name__ == "__main__":
    sys.exit(main())