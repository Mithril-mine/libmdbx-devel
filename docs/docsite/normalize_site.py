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
import datetime
import hashlib
import os
import posixpath
import re
import shutil
import sys
import urllib.parse
from pathlib import Path

_here = Path(__file__).resolve().parent
SITE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else _here / ".." / ".." / "build" / "docsite"
LOCALES = ("en", "ru")
# canonical origin and /docs/ base from docs-integration-contract §2
SITE_URL = "https://libmdbx.dqdkfa.ru"
DOCS_URL = SITE_URL + "/docs"
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
    """One shared copy of the theme assets at site/assets. Idempotent: when
    mkdocs re-created the per-locale copies over an already-normalized tree,
    they are verified against the shared copy and dropped."""
    en, ru = SITE / "en" / "assets", SITE / "ru" / "assets"
    shared = SITE / "assets"
    if not en.exists() and not ru.exists():
        if not shared.exists():
            fail("no assets found in the site tree")
        return  # already normalized
    # byte-identical check of the two locales (and of re-created copies
    # against an existing shared copy)
    def files_of(root):
        return {p.relative_to(root): sha256(p) for p in root.rglob("*")
                if p.is_file()}
    en_files = files_of(en) if en.exists() else files_of(shared)
    ru_files = files_of(ru) if ru.exists() else files_of(shared)
    if en_files != ru_files:
        diff = {k for k in set(en_files) ^ set(ru_files)}
        diff |= {k for k in en_files.keys() & ru_files.keys()
                 if en_files[k] != ru_files[k]}
        fail(f"assets differ between locales: {sorted(diff)[:5]} ...")
    if shared.exists():
        # the shared copy is a normalize product (it carries the search-index
        # patch); the fresh per-locale copies from the build are the truth -
        # replace the shared copy wholesale
        shutil.rmtree(shared)
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


def localize_canonical_links():
    """Content links may use full canonical URLs (mkdocs skips them silently,
    so cross-locale and mkdoxy-generated links produce no validation noise).
    After the build, those are converted to relative paths - EXCEPT the
    contract-owned absolute URLs: hreflang <link>s in <head> and JSON-LD
    blocks keep the full https:// form (§2, §3.1, §4.1, §4.3). Idempotent."""
    fixed = 0
    checked = 0
    for html in sorted(SITE.rglob("*.html")):
        text = html.read_text(encoding="utf-8")

        def fix(m):
            nonlocal fixed, checked
            checked += 1
            abs_url = m.group(2)
            rel = urllib.parse.urlparse(abs_url).path[len("/docs/"):]
            target = (SITE / rel).resolve()
            if not target.is_file():
                return m.group(0)  # not a built page: keep the absolute URL
            local = os.path.relpath(target, html.parent)
            fixed += 1
            return f'{m.group(1)}"{local}"'

        new = re.sub(rf'(<(?:a|img)[^>]*?\b(?:href|src)=)"'
                     rf'({re.escape(DOCS_URL)}/[^"]*)"', fix, text)
        if new != text:
            html.write_text(new, encoding="utf-8")
    print(f"  localize: {checked} canonical content links checked, "
          f"{fixed} made relative")


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


def prune_theme_leftovers():
    """Material ships its default favicon as a theme static file, which mkdocs
    copies into the output on every build even though theme.favicon points at
    the shared /static-content/ icons. Nothing references the copy - prune it.
    Idempotent."""
    removed = 0
    images = SITE / "assets" / "images"
    if images.is_dir():
        for name in ("favicon.png", "favicon.ico"):
            f = images / name
            if f.exists():
                f.unlink()
                removed += 1
        if not any(images.iterdir()):
            images.rmdir()
    if removed:
        print(f"  prune: dropped {removed} unused default theme favicon(s)")


SITEMAP_INDEX_STUB = """<?xml version="1.0" encoding="utf-8"?>
<sitemapindex xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <sitemap>
    <loc>{root}</loc>
  </sitemap>
</sitemapindex>
"""


def _content_page_dirs():
    """Directories containing content pages, locale-root-relative; Material's
    alternate integration fetches sitemap.xml in the directory of every
    link[rel=alternate] href, i.e. in the directory of every content page."""
    dirs = set()
    for html in SITE.rglob("*.html"):
        rel = html.relative_to(SITE).as_posix()
        loc = rel.split("/", 1)[0]
        if loc not in LOCALES or rel == f"{loc}/404.html":
            continue
        dirs.add(posixpath.dirname(rel))
    dirs.discard("")  # the docsite root carries the canonical sitemap
    return sorted(dirs)


def emit_locale_sitemaps():
    """Material fetches sitemap.xml at the site root (config.base) on every
    page - see the `sitemap$` wiring for instant navigation and previews -
    so each locale root gets a copy of the canonical root sitemap. The
    mkdocs-generated per-locale sitemaps are replaced; the .gz archives are
    dropped."""
    for loc in LOCALES:
        gz = SITE / loc / "sitemap.xml.gz"
        if gz.exists():
            gz.unlink()
        shutil.copyfile(SITE / "sitemap.xml", SITE / loc / "sitemap.xml")
    print(f"  sitemap: locale-root copies emitted ({LOCALES[0]}/{LOCALES[1]})")


def emit_directory_stubs():
    """Per-directory sitemap-index stubs pointing at the canonical root
    sitemap: without them the Material alternate integration gets a 404 for
    every content page directory (two per page view). A sitemapindex yields
    an empty sitemap for the parser - silent - and is a semantically correct
    pointer for any crawler stumbling on the file. Idempotent."""
    count = 0
    for d in _content_page_dirs():
        stub = SITE / d / "sitemap.xml"
        if not stub.exists():
            stub.write_text(
                SITEMAP_INDEX_STUB.format(root=f"{DOCS_URL}/sitemap.xml"),
                encoding="utf-8")
            count += 1
    print(f"  sitemap: {count} directory stub(s) -> canonical root")


SELECTOR_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="application-name" content="libmdbx">
<title>libmdbx documentation</title>
<link rel="shortcut icon" href="/static-content/favicon.ico">
<link rel="icon" href="/static-content/favicon.ico" sizes="any">
<link rel="icon" href="/static-content/icon.svg" type="image/svg+xml">
<link rel="icon" href="/static-content/favicon-32x32.png" type="image/png" sizes="32x32">
<link rel="icon" href="/static-content/favicon-16x16.png" type="image/png" sizes="16x16">
<link rel="icon" href="/static-content/icon-192.png" type="image/png" sizes="192x192">
<link rel="apple-touch-icon" href="/static-content/apple-touch-icon.png" sizes="180x180" type="image/png">
<link rel="manifest" href="/static-content/manifest.webmanifest">
<style>
body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
       display: flex; min-height: 100vh; margin: 0; }}
nav {{ margin: auto; text-align: center; }}
h1 {{ font-weight: 400; }}
a.lang {{ display: inline-block; margin: .5em 1em; padding: .6em 2.2em;
          border: 1px solid #888; border-radius: .4em;
          font-size: 1.2em; text-decoration: none; color: inherit; }}
a.lang:hover {{ border-color: currentColor; }}
</style>
</head>
<body>
<!-- Яндекс Метрика -->
<script type="text/javascript" >
   (function(m,e,t,r,i,k,a){m[i]=m[i]||function(){(m[i].a=m[i].a||[]).push(arguments)};
   m[i].l=1*new Date();k=e.createElement(t),a=e.getElementsByTagName(t)[0],k.async=1,k.src=r,a.parentNode.insertBefore(k,a)})
   (window, document, "script", "https://mc.yandex.ru/metrika/tag.js", "ym");

   ym(99261645, "init", {
       clickmap:true,
       trackLinks:true,
       accurateTrackBounce:true,
       webvisor:true
   });
</script>
<noscript><div><img src="https://mc.yandex.ru/watch/99261645" style="position:absolute; left:-9999px;" alt="" /></div></noscript>
<!-- /Яндекс Метрика -->
<nav>
<h1>libmdbx documentation</h1>
<p><a class="lang" href="/docs/ru/index.html" lang="ru">Русский</a>
   <a class="lang" href="/docs/en/index.html" lang="en">English</a></p>
</nav>
<script>
// Auto-redirect per docs-integration-contract: only when the language
// preference clearly derives from the FIRST entry of navigator.languages
// (ru* -> RU, en* -> EN, otherwise stay on the selector). A manual choice
// stored in localStorage overrides the browser preference; a query string
// or hash disables the auto-redirect entirely.
(function () {{
  "use strict";
  if (location.search || location.hash) return;
  try {{
    var saved = localStorage.getItem("docs:lang");
    if (saved === "ru" || saved === "en") {{
      location.replace("/docs/" + saved + "/index.html");
      return;
    }}
    var langs = navigator.languages || [navigator.language || "en"];
    var first = (langs[0] || "en").toLowerCase();
    if (first.indexOf("ru") === 0)
      location.replace("/docs/ru/index.html");
    else if (first.indexOf("en") === 0)
      location.replace("/docs/en/index.html");
  }} catch (e) {{ /* stay on the selector */ }}
}})();
</script>
</body>
</html>
"""


def make_selector():
    """The language-selector landing at the docsite root: the single target
    of the API-gateway redirect (docs/ -> docs/index.html). Not a content
    page: no TechArticle/BreadcrumbList, and it enters the sitemap without
    hreflang annotations (§3.1)."""
    target = SITE / "index.html"
    target.write_text(SELECTOR_TEMPLATE, encoding="utf-8")
    print(f"  selector: {target.relative_to(SITE.parent)}")


def _lastmod_map():
    """Page URL -> YYYY-MM-DD from the hook manifest; pages missing from it
    (e.g. the selector) fall back to today."""
    manifest = SITE / ".lastmod.json"
    try:
        return json.loads(manifest.read_text(encoding="utf-8"))
    except Exception:
        return {}


def make_sitemap():
    """docs-integration-contract §3/§3.1: one sitemap, both locales, full
    object paths, hreflang xhtml:link pairs + x-default -> the selector."""
    import xml.etree.ElementTree as ET

    NS = "http://www.sitemaps.org/schemas/sitemap/0.9"
    XHTML = "http://www.w3.org/1999/xhtml"
    ET.register_namespace("xhtml", XHTML)
    today = datetime.date.today().strftime("%Y-%m-%d")
    lastmod = _lastmod_map()

    pages = []
    for html in sorted(SITE.rglob("*.html")):
        rel = html.relative_to(SITE).as_posix()
        loc = rel.split("/", 1)[0]
        if loc not in LOCALES:
            continue  # the language selector (index.html) at the docsite root
        if rel == f"{loc}/404.html":
            continue  # the error page
        pages.append(rel)

    root = ET.Element("urlset", xmlns=NS)
    count = 0
    for rel in pages:
        loc = rel.split("/", 1)[0]
        other = "ru" if loc == "en" else "en"
        url = ET.SubElement(root, "url")
        ET.SubElement(url, "loc").text = f"{DOCS_URL}/{rel}"
        ET.SubElement(url, "lastmod").text = lastmod.get(rel, today)
        if rel.startswith(("en/reference/api/", "en/reference/changelog.html")):
            # EN-only pages (contract amendment): hreflang en + x-default only
            ET.SubElement(url, f"{{{XHTML}}}link",
                          rel="alternate", hreflang="en",
                          href=f"{DOCS_URL}/{rel}")
        else:
            twin = f"{other}/{rel.split('/', 1)[1]}"
            for lang, href in ((loc, rel), (other, twin)):
                ET.SubElement(url, f"{{{XHTML}}}link",
                              rel="alternate", hreflang=lang,
                              href=f"{DOCS_URL}/{href}")
        ET.SubElement(url, f"{{{XHTML}}}link",
                      rel="alternate", hreflang="x-default",
                      href=f"{DOCS_URL}/index.html")
        count += 1
    # the language selector: present without xhtml:link annotations (§3.1)
    url = ET.SubElement(root, "url")
    ET.SubElement(url, "loc").text = f"{DOCS_URL}/index.html"
    ET.SubElement(url, "lastmod").text = today

    ET.indent(root)
    path = SITE / "sitemap.xml"
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)
    print(f"  sitemap: {count + 1} URLs -> {path.relative_to(SITE.parent)}")


def main():
    if not SITE.exists():
        fail(f"{SITE} not found - run `make docsite` (mkdocs) first")
    purge_doxy_junk()
    dedupe_assets()
    prune_theme_leftovers()
    relocate_search()
    rewrite_links()
    localize_canonical_links()
    audit_links()
    make_selector()
    make_sitemap()
    emit_locale_sitemaps()
    emit_directory_stubs()
    total = sum(1 for _ in SITE.rglob("*") if _.is_file())
    print(f"  normalize: done ({total} files under {SITE})")


if __name__ == "__main__":
    main()
