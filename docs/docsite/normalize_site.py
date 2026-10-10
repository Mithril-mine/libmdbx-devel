#!/usr/bin/env python3
"""Post-build normalizer for the unified libmdbx docsite (make docsite).

Generalization of docs/textbook/normalize_site.py for the two-locale docsite
(site/{en,ru}). Runs after both MkDocs builds and reshapes the output for
S3-bucket hosting (no directory -> index.html rewrites available):

0. junk:     site/{loc}/assets/.doxy (mkdoxy's doxygen XML cache) is removed
             - the generated pages are already rendered HTML;
1. assets:   site/{loc}/assets must be identical -> moved to site/assets
             (single copy), per-locale copies removed;
2. search:   site/{loc}/search/search_index.json -> site/{loc}/search_index.json
             (the search/ subdir disappears), and the Material bundle is patched
             to fetch "search_index.json" instead of "search/search_index.json";
3. rewrite:  every href/src/srcset in generated .html is adjusted so it
             resolves to an existing file under the final tree;
4. audit:    all links must address existing FILES (never a directory, never a
             404) - S3 static hosting does not resolve either. Violations are
             reported and make the script exit non-zero.

Unlike the textbook normalizer there is no examples/examples dedupe: the
docsite keeps per-locale copies of the textbook example trees. The script is
idempotent: re-running on an already normalized tree is a no-op.

Usage: python3 normalize_site.py [site-root]   (default: next to this file/site)
"""
import hashlib
import os
import re
import shutil
import sys
import urllib.parse
from pathlib import Path

_here = Path(__file__).resolve().parent
SITE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else _here / ".." / ".." / "build" / "docsite"
LOCALES = ("en", "ru")
REF_ATTRS = re.compile(r'\b(href|src|srcset|poster)="([^"]*)"')


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fail(msg):
    print(f"  NORMALIZE ERROR: {msg}")
    sys.exit(1)


def purge_doxy_junk():
    """Remove mkdoxy's doxygen work area (XML + generated md sources) from the
    output: the pages are already rendered, the XML is build input, not
    site content. Idempotent."""
    removed = 0
    for loc in LOCALES:
        junk = SITE / loc / "assets" / ".doxy"
        if junk.exists():
            shutil.rmtree(junk)
            removed += 1
    if removed:
        print(f"  junk: removed mkdoxy work area from {removed} locale(s)")


def dedupe_assets():
    """One shared copy of the theme assets at site/assets."""
    en, ru = SITE / "en" / "assets", SITE / "ru" / "assets"
    shared = SITE / "assets"
    if not en.exists() and shared.exists():
        return  # already normalized
    if not en.exists() or not ru.exists():
        fail("expected site/en/assets and site/ru/assets to both exist")
    # byte-identical check
    en_files = {p.relative_to(en): sha256(p) for p in en.rglob("*") if p.is_file()}
    ru_files = {p.relative_to(ru): sha256(p) for p in ru.rglob("*") if p.is_file()}
    if en_files != ru_files:
        diff = {k for k in set(en_files) ^ set(ru_files)}
        diff |= {k for k in en_files.keys() & ru_files.keys()
                 if en_files[k] != ru_files[k]}
        fail(f"assets differ between locales: {sorted(diff)[:5]} ...")
    shutil.move(str(en), str(shared))
    shutil.rmtree(ru)
    print(f"  assets: deduped -> {shared} ({len(en_files)} files)")


def relocate_search():
    """site/{loc}/search/search_index.json -> site/{loc}/search_index.json and
    patch the Material bundle to fetch the relocated index. Idempotent: skips
    when the index is already relocated and the bundle already patched."""
    moved = False
    for loc in LOCALES:
        old = SITE / loc / "search" / "search_index.json"
        new = SITE / loc / "search_index.json"
        if old.exists():
            shutil.move(str(old), str(new))
            shutil.rmtree(SITE / loc / "search", ignore_errors=True)
            moved = True
            print(f"  search: {loc}/search/search_index.json -> {loc}/search_index.json")
    bundle = next((SITE / "assets" / "javascripts").glob("bundle.*.min.js"), None)
    if bundle is None:
        fail("Material bundle javascripts/bundle.*.min.js not found")
    text = bundle.read_text(encoding="utf-8")
    if '"search/search_index.json"' not in text:
        if '"search_index.json"' in text:
            return  # already patched on a previous run
        fail("bundle search-index loader pattern not found - Material theme changed?")
    patched, n_json = re.subn(r'"search/search_index\.json"', '"search_index.json"', text)
    patched, n_js = re.subn(r'"search/search_index\.js"', '"search_index.js"', patched)
    if n_json != 1 or n_js != 1:
        fail(f"bundle search-index loader pattern not found "
             f"(json x{n_json}, js x{n_js}) - Material theme changed?")
    bundle.write_text(patched, encoding="utf-8")
    print(f"  search: bundle patched ({bundle.name})"
          f"{'' if moved else ' (index already relocated)'}")


def rewrite_links():
    """Make every local href/src/srcset resolve to an existing file under the
    final tree: insert one more '../' where the asset hoist shifted depth."""
    fixed = checked = 0
    for html in sorted(SITE.rglob("*.html")):
        base = html.parent
        text = html.read_text(encoding="utf-8")

        def fix(m):
            nonlocal fixed, checked
            attr, raw = m.group(1), m.group(2)
            if not raw or raw.startswith(("#", "http:", "https:", "mailto:",
                                          "data:", "javascript:")):
                return m.group(0)
            checked += 1
            target = urllib.parse.unquote(raw.split("#", 1)[0].split("?", 1)[0])
            resolved = (base / target).resolve()
            if resolved.is_file():
                return m.group(0)
            # the hoist pushed assets one level up: retry with an extra '../'
            # inserted at the front of the relative path
            lifted = "../" + target
            if (base / lifted).resolve().is_file():
                fixed += 1
                return f'{attr}="{lifted}"'
            # mkdoxy quirk: the related-pages listing links the main doxygen
            # page by its refid (indexpage.md) while the page file is index.md
            if target == "indexpage.md" and (base / "index.html").is_file():
                fixed += 1
                return f'{attr}="index.html"'
            return m.group(0)  # leave untouched; the audit reports it

        new = REF_ATTRS.sub(fix, text)
        if new != text:
            html.write_text(new, encoding="utf-8")
    print(f"  links: {checked} refs checked, {fixed} adjusted for the hoist")


def audit_links():
    """Every local link must address an existing FILE; directory-style links
    (trailing '/') are errors: S3 hosting does not append index.html.
    Site-absolute links (/...) are mkdocs site_url-derived and are verified
    only for being file-shaped, not for deployment-prefix correctness."""
    bad_dir, bad_missing = [], []
    for html in sorted(SITE.rglob("*.html")):
        base = html.parent
        for m in REF_ATTRS.finditer(html.read_text(encoding="utf-8")):
            raw = m.group(2)
            if not raw or raw.startswith(("#", "http:", "https:", "mailto:",
                                          "data:", "javascript:")):
                continue
            if raw.endswith("/"):
                bad_dir.append(f"{html.relative_to(SITE)}: {raw}")
                continue
            if raw.startswith("/"):
                continue  # deployment-absolute; resolved by the hosting prefix
            target = urllib.parse.unquote(raw.split("#", 1)[0].split("?", 1)[0])
            if not (base / target).resolve().is_file():
                bad_missing.append(f"{html.relative_to(SITE)}: {raw}")
    if bad_dir or bad_missing:
        for line in (bad_dir + bad_missing)[:20]:
            print(f"  AUDIT FAIL: {line}")
        fail(f"{len(bad_dir)} directory-style links, "
             f"{len(bad_missing)} dangling links")
    print(f"  audit: all local links resolve to files "
          f"(0 directory-style, 0 dangling)")


def main():
    if not SITE.exists():
        fail(f"{SITE} not found - run `make docsite` (mkdocs) first")
    purge_doxy_junk()
    dedupe_assets()
    relocate_search()
    rewrite_links()
    audit_links()
    total = sum(1 for _ in SITE.rglob("*") if _.is_file())
    print(f"  normalize: done ({total} files under {SITE})")


if __name__ == "__main__":
    main()
