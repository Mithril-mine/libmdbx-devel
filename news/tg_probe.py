#!/usr/bin/env python3
"""skynet telegram probe — check bot + read updates + reply to owner (TASK-32).

Runs inside GitHub Actions where TELEGRAM_BOT_TOKEN is available as a secret
(the local host cannot reach api.telegram.org). One-shot:
  1. getMe  — verify the token/bot.
  2. getUpdates (offset=-N) — list recent updates; find the LATEST private
     chat message addressed to the bot (chat.type == 'private'), prefer one
     from the owner's user_id if known.
  3. sendMessage — reply to that private chat ONLY (never to the group) with
     a short "skynet here" ack, unless --dry-run.

Env:
  TELEGRAM_BOT_TOKEN  required
  TG_OWNER_ID         optional; if set, a message from this user is preferred
  TG_API_BASE         optional; default https://api.telegram.org

Exit: 0 ok (or dry-run), 1 failure.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

API_BASE = os.environ.get("TG_API_BASE", "https://api.telegram.org")


def api(token, method, params=None):
    url = f"{API_BASE}/bot{token}/{method}"
    data = json.dumps(params or {}).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        raise RuntimeError(f"telegram {method} HTTP {e.code}: {body}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"telegram {method} network: {e.reason}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="read-only, do not send")
    ap.add_argument("--owner-id", default=os.environ.get("TG_OWNER_ID"))
    ap.add_argument("--limit", type=int, default=20, help="getUpdates limit")
    args = ap.parse_args()

    token = os.environ.get("TELEGRAM_BOT_TOKEN")
    if not token:
        print("ERR: TELEGRAM_BOT_TOKEN unset", file=sys.stderr)
        return 1

    me = api(token, "getMe")
    print(f"OK getMe: {me['result']['username']} id={me['result']['id']}")
    print(f"   can_read_all_group_messages={me['result'].get('can_read_all_group_messages')}")

    upd = api(token, "getUpdates", {"limit": args.limit, "timeout": 0})
    msgs = upd.get("result", [])
    print(f"OK getUpdates: {len(msgs)} update(s)")
    for u in msgs[-args.limit:]:
        m = u.get("message") or u.get("channel_post") or {}
        ch = m.get("chat", {})
        fr = m.get("from", {})
        txt = (m.get("text") or "")[:80].replace("\n", " ")
        print(f"   u{u['update_id']} [{ch.get('type')}] chat={ch.get('id')} "
              f"from={fr.get('id')} ({fr.get('first_name')}) :: {txt}")

    # pick the newest private-chat message
    target = None
    for u in reversed(upd.get("result", [])):
        m = u.get("message") or {}
        ch = m.get("chat", {})
        if ch.get("type") == "private":
            target = ch.get("id")
            break

    if target is None:
        print("WARN: no private-chat message found; nothing to reply to.")
        if args.dry_run:
            return 0
        return 1

    if args.dry_run:
        print(f"DRY-RUN: would reply to private chat {target}")
        return 0

    reply = ("Skynet here! Telegram bridge is up. "
             "Я — координатор роя ИИ-агентов libmdbx; это тестовое подтверждение "
             "канала (в группу пока не пишу).")
    sent = api(token, "sendMessage", {"chat_id": target, "text": reply})
    print(f"OK sendMessage -> {sent['result']['chat']['id']} mid={sent['result']['message_id']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())