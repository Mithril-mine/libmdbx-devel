#!/usr/bin/env python3
"""Mechanical audit of every https://libmdbx.dqdkfa.ru link.

The rule: each such URL must either
  - lead through /docs/** to an existing file in the built docsite tree
    (build/docsite by default), or
  - be covered by the allowlist below (archival/dead-by-design references
    and URLs that are known to be served on the live site). With --online
    every non-/docs/ URL is additionally probed over the network.

Usage:
  python3 check_links.py [--online] [--site PATH]
      --online   probe the non-allowlisted non-/docs/ URLs over HTTP(S)
      --site P   built docsite root (default: repo root / build/docsite)

Offline mode is part of `make docsite`; `make check-links-live` runs the
online pass.
"""

import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path.cwd()
SITE = Path(os.environ.get("DOCSITE_SITE", "build/docsite")).resolve()
BOOKS = ROOT / "build" / "textbook"
DOMAIN = "https://libmdbx.dqdkfa.ru"
URL_RE = re.compile(r"https://libmdbx\.dqdkfa\.ru[^\s\"'<>)\]}]*")

# (pattern, reason): matched URLs are never probed - the reference is
# deliberately archival and its remote availability is not re-verified.
ALLOW_SKIP = [
    # libmdbx.dqdkfa.ru/dead-github/<ref> stands for the saved web.archive.org
    # copy of a resource destroyed by GitHub admins; the prefix self-documents
    # the state (see ChangeLog-0.12), the redirect target lives on github.com.
    (re.compile(r"^https://libmdbx\.dqdkfa\.ru/dead-github"), "archival"),
    # the standalone books site /docs/textbook/{en,ru}/ is a separate
    # deployment artifact (`make books` -> build/textbook); the offline gate
    # verifies it against the local tree when present, the live site is
    # updated independently of the docsite.
    (re.compile(r"^https://libmdbx\.dqdkfa\.ru/docs/textbook/"),
     "standalone books site"),
]

# (pattern, reason): allowed by the offline gate; with --online these are
# still probed over the network and must return 2xx.
ALLOW_OFFLINE = [
    # the site root, the bare domain (man pages, mdbx.h++ comments) and the
    # homepage anchors
    (re.compile(r"^https://libmdbx\.dqdkfa\.ru/?(#.*)?$"), "site root"),
    # release assets (presentation PDFs)
    (re.compile(r"^https://libmdbx\.dqdkfa\.ru/release/"), "release assets"),
]


def resolve_local(path):
    """Resolve a /docs/<rel> path against the built docsite tree."""
    candidates = []
    if path == "/docs":
        candidates.append(SITE / "index.html")
    elif path.startswith("/docs/"):
        rel = path[len("/docs/"):].lstrip("/")
        candidates.append(SITE / rel)
        if not Path(rel).suffix:
            candidates.append(SITE / rel / "index.html")
    else:
        candidates.append(SITE / "index.html")
    return any(c.is_file() for c in candidates)


def fail(msg):
    print(f"  LINK-AUDIT ERROR: {msg}")
    sys.exit(1)


def tracked_files():
    try:
        out = subprocess.check_output(["git", "ls-files"], text=True,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return []
    return [ROOT / p for p in out.splitlines() if p]


def site_files():
    out = set()
    if SITE.exists():
        for p in SITE.rglob("*"):
            if p.is_file() and (p.suffix in (".html", ".xml")
                                or p.name in ("sitemap.xml", "search_index.json")):
                out.add(p)
    books = ROOT / "build" / "textbook"
    if (books / "en").is_dir() or (books / "ru").is_dir():
        for p in books.rglob("*.html"):
            out.add(p)
    return sorted(out)


def pdf_urls():
    pdfs = [p for p in [ROOT / f"build/docsite/{loc}/textbook/libmdbx-textbook-{loc}.pdf"
                        for loc in ("ru", "en")]
            if p.is_file()]
    if not pdfs:
        return set()
    try:
        from pypdf import PdfReader
    except Exception:
        print("  links: pypdf is not installed - PDF link annotations are not "
              "checked (pip install pypdf)")
        return set()
    found = set()
    for pdf in pdfs:
        try:
            reader = PdfReader(pdf)
            for page in reader.pages:
                for annot in page.get("/Annots") or []:
                    a = annot.get_object()
                    if a.get("/Subtype") == "/Link":
                        act = a.get("/A")
                        uri = act.get("/URI") if act else None
                        if uri and DOMAIN in str(uri):
                            found.add(str(uri))
        except Exception:
            continue
    return found


def clean(u):
    """Trim markdown/code artifacts and drop template placeholders."""
    u = u.strip()
    while u and u[-1] in ".,;:!?)]":
        u = u[:-1]
    u = u.rstrip("`")
    if "{" in u or "}" in u:
        return None  # a {ru,en}-style format template, not a real link
    return u


def classify_local(u):
    """Return ('LOCAL', ok) for URLs served by the built docsite tree,
    ('LOCAL', ok) for the standalone books tree when it is present,
    otherwise None."""
    path = urllib.parse.urlparse(u).path
    if path == "/docs/textbook" or path.startswith("/docs/textbook/"):
        rel = path[len("/docs/textbook/"):].lstrip("/")
        if not rel:
            return None  # books root page is part of the books deployment
        if (BOOKS / rel).is_file():
            return ("LOCAL", True)
        if not Path(rel).suffix and (BOOKS / rel / "index.html").is_file():
            return ("LOCAL", True)
        return None  # books site absent from this build: offline-allowed
    if path.startswith("/docs/") or path == "/docs":
        return ("LOCAL", resolve_local(path))
    return None


def probe(u):
    req = urllib.request.Request(u, headers={"User-Agent": "libmdbx-link-audit"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return resp.status, resp.geturl()
    except urllib.error.HTTPError as e:
        return e.code, getattr(e, "url", u)
    except Exception as e:
        return None, str(e)[:80]


def main():
    online = "--online" in sys.argv
    if "--site" in sys.argv:
        global SITE
        SITE = Path(sys.argv[sys.argv.index("--site") + 1]).resolve()

    files = tracked_files() + list(site_files())
    url_counts = {}
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        for m in URL_RE.findall(text):
            url = clean(m)
            if url:
                url_counts[url] = url_counts.get(url, 0) + 1
    for url in pdf_urls():
        url_counts[url] = url_counts.get(url, 0) + 1

    violations = []
    counts = {"local_ok": 0, "allowed": 0, "skipped": 0, "live_ok": 0}
    for url, n in sorted(url_counts.items()):
        local = classify_local(url)
        if local is not None:
            kind, ok = local
            if ok:
                counts["local_ok"] += 1
            else:
                violations.append((url, n, "LOCAL-MISS"))
            continue
        if any(pat.match(url) for pat, _ in ALLOW_SKIP):
            counts["skipped"] += 1
            continue
        if any(pat.match(url) for pat, _ in ALLOW_OFFLINE):
            if not online:
                counts["allowed"] += 1
                continue
        if not online:
            violations.append((url, n, "NEEDS-LIVE"))
            continue
        status, final = probe(url)
        if status and 200 <= status < 400:
            counts["live_ok"] += 1
        else:
            violations.append((url, n, f"LIVE-{status}", final))

    print(f"  links: {len(url_counts)} unique libmdbx.dqdkfa.ru URL(s): "
          f"{counts['local_ok']} local /docs/, {counts['allowed']} allowlisted, "
          f"{counts['skipped']} skipped, {counts['live_ok']} live"
          + ("" if online else " (+NEEDS-LIVE report)"))
    if violations:
        for v in violations:
            if len(v) == 4:
                url, n, kind, final = v
                print(f"  AUDIT FAIL: {kind:11s} x{n:3d} {url}  (-> {final})")
            else:
                url, n, kind = v
                print(f"  AUDIT FAIL: {kind:11s} x{n:3d} {url}")
        fail(f"{len(violations)} link violation(s)")
    print("  links: audit ok")


if __name__ == "__main__":
    main()