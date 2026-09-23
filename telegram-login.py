#!/usr/bin/env python3
"""Telegram MTProto login helper (single phase, code via SMS).

Runs on GitHub Actions runner (has network to Telegram, unlike dev host).
Phase logic in ONE workflow with two dispatch runs:
  run #1: --request-code <phone>   -> sends SMS code, saves session file
  run #2: --sign-in <phone> <code> -> completes login, prints session b64

Requires: api_id/api_hash (from my.telegram.org) via env TELEGRAM_API_ID/
TELEGRAM_API_HASH, and Telethon. Session is stored to a local file and printed
as base64 for saving into a GitHub Secret.

Security: phone/code are arguments ONLY inside a GitHub Actions dispatch, never
in git. The session file (a logged-in user session) is the secret; save it as
TELEGRAM_SESSION_B64 and treat like a token.
"""

import argparse
import base64
import os
import sys


def ensure_telethon():
    try:
        import telethon  # noqa: F401
        return True
    except ImportError:
        return False


def get_api_creds():
    aid = os.environ.get("TELEGRAM_API_ID", "")
    ahash = os.environ.get("TELEGRAM_API_HASH", "")
    if not aid or not ahash:
        sys.exit("TELEGRAM_API_ID / TELEGRAM_API_HASH env vars required")
    return int(aid), ahash


def session_path():
    return "/tmp/skynet.session"


def request_code(phone):
    from telethon import TelegramClient
    from telethon.errors import ApiIdInvalidError

    api_id, api_hash = get_api_creds()
    client = TelegramClient(session_path(), api_id, api_hash)
    try:
        client.start(phone=phone, code_callback=lambda: None)  # will fail on code
    except ApiIdInvalidError:
        sys.exit("api_id/api_hash invalid")
    except Exception as e:
        # Expected: we need the code, but session must be saved BEFORE sign-in.
        # Telethon request_code path: use send_code_request explicitly.
        pass
    sent = client.send_code_request(phone)
    client.session.save()
    print("CODE-SENT to %s (phone_code_hash=%s...)" % (phone, sent.phone_code_hash[:8]))
    print("SESSION-SAVED (pending login). Run #2: sign-in with the SMS code.")


def sign_in(phone, code):
    from telethon import TelegramClient
    from telethon.tl.functions.auth import SignInRequest
    from telethon.tl.types import PhoneCodeHash

    api_id, api_hash = get_api_creds()
    client = TelegramClient(session_path(), api_id, api_hash)
    client.connect()
    # Resolve pending code hash from the saved session.
    try:
        result = client.sign_in(phone=phone, code=code)
    except Exception as e:
        sys.exit("sign_in failed: %s" % e)
    client.session.save()
    b64 = base64.b64encode(open(session_path(), "rb").read()).decode()
    print("SIGNED-IN as %s %s" % (getattr(result, 'first_name', '?'),
                                  getattr(result, 'last_name', '') or ''))
    print("TELEGRAM_SESSION_B64_START")
    print(b64)
    print("TELEGRAM_SESSION_B64_END")
    print("Save the base64 above as the TELEGRAM_SESSION_B64 GitHub Secret.")


def main():
    ap = argparse.ArgumentParser(description="Telegram MTProto login (single phase)")
    ap.add_argument("--request-code", metavar="PHONE")
    ap.add_argument("--sign-in", nargs=2, metavar=("PHONE", "CODE"))
    args = ap.parse_args()
    if not ensure_telethon():
        sys.exit("telethon not installed (pip install telethon)")
    if args.request_code:
        request_code(args.request_code)
    elif args.sign_in:
        sign_in(*args.sign_in)
    else:
        ap.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()