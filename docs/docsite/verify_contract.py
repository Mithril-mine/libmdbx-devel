#!/usr/bin/env python3
"""Verify the docsite against the docs-integration-contract checklist (§6).

Checks (§6 items in brackets):
  [1] /docs/sitemap.xml is a valid 0.9 sitemap with xhtml namespace, only
      200-URLs (every <loc> resolves to a file in the built tree), lastmod
      is W3C date-formatted;
  [2] every content page carries a JSON-LD block with TechArticle and
      BreadcrumbList (@graph welcome);
  [3] the @id entity references match §2 verbatim;
  [4] no page re-defines Organization/WebSite/SoftwareSourceCode entities;
  [5] dateModified is date-formatted (its content correctness is the
      generator's guarantee: git last-commit dates);
  [6] RU<->EN pairs: every page of one locale has the twin in the other,
      except EN-only pages (reference/api/**, reference/changelog.html) and
      the language selector;
  [7] triple consistency of hreflang data: <head> links (§4.3) == sitemap
      xhtml:link (§3.1) == ld+json translation fields (§4.1);
  [8] inLanguage matches the actual content locale;
  [9] no bare directory-URLs anywhere; x-default -> /docs/index.html.

Usage: python3 verify_contract.py [site-root]   (default: build/docsite)
"""
import json
import re
import sys
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path

_here = Path(__file__).resolve().parent
SITE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else _here / ".." / ".." / "build" / "docsite"
LOCALES = ("en", "ru")
SITE_URL = "https://libmdbx.dqdkfa.ru"
DOCS_URL = SITE_URL + "/docs"
NS = "{http://www.sitemaps.org/schemas/sitemap/0.9}"
XHTML = "{http://www.w3.org/1999/xhtml}"
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

errors = []


def fail(msg):
    errors.append(msg)


def content_pages():
    """All content pages of both locales as (locale, relpath) with the page
    URL relative to the docsite root."""
    pages = {}
    for loc in LOCALES:
        for html in sorted((SITE / loc).rglob("*.html")):
            rel = html.relative_to(SITE).as_posix()
            if rel.endswith("/404.html"):
                continue
            pages[rel] = html
    return pages


def parse_ld(html: Path):
    blocks = re.findall(
        r'<script type="application/ld\+json">\s*(.*?)\s*</script>', html.read_text(encoding="utf-8"), re.S)
    out = []
    for b in blocks:
        try:
            out.append(json.loads(b))
        except json.JSONDecodeError as e:
            fail(f"{html.relative_to(SITE)}: JSON-LD is not valid JSON: {e}")
    return out


def graph_of(blocks):
    nodes = []
    for b in blocks:
        if "@graph" in b:
            nodes.extend(b["@graph"])
        else:
            nodes.append(b)
    return nodes


def check_sitemap(pages):
    path = SITE / "sitemap.xml"
    if not path.is_file():
        fail("sitemap.xml is missing at the docsite root")
        return {}
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        fail(f"sitemap.xml is not valid XML: {e}")
        return {}
    if root.tag != NS + "urlset":
        fail("sitemap.xml root is not a 0.9 urlset")
        return {}
    entries = {}
    for url in root.findall(NS + "url"):
        loc = url.findtext(NS + "loc") or ""
        rel = urllib.parse.urlparse(loc).path[len("/docs/"):]
        lastmod = url.findtext(NS + "lastmod") or ""
        if not DATE_RE.match(lastmod):
            fail(f"sitemap: bad lastmod '{lastmod}' for {loc}")
        if loc.endswith("/"):
            fail(f"sitemap: directory-style URL {loc} (§6.9)")
        if not (SITE / rel).is_file():
            fail(f"sitemap: <loc> {loc} does not resolve to a built file (§6.1)")
        alts = {}
        for link in url.findall(XHTML + "link"):
            lang, href = link.get("hreflang"), link.get("href", "")
            alts[lang] = href
            if href.endswith("/"):
                fail(f"sitemap: directory-style href for {lang} in {loc}")
        if "x-default" not in alts and loc != f"{DOCS_URL}/index.html":
            fail(f"sitemap: {loc} has no x-default")
        elif loc != f"{DOCS_URL}/index.html" and \
                alts.get("x-default") != f"{DOCS_URL}/index.html":
            fail(f"sitemap: {loc} x-default must be the selector (§6.9)")
        entries[loc] = alts
    # the selector itself must be present, without xhtml:link annotations
    if f"{DOCS_URL}/index.html" not in entries:
        fail("sitemap: the language selector /docs/index.html is missing")
    elif entries[f"{DOCS_URL}/index.html"]:
        fail("sitemap: the selector must not carry xhtml:link annotations")
    return entries


def check_pages(pages, sitemap):
    for rel, html in pages.items():
        loc = rel.split("/", 1)[0]
        name = html.relative_to(SITE).as_posix()
        canonical = f"{DOCS_URL}/{rel}"
        nodes = graph_of(parse_ld(html))
        articles = [n for n in nodes if n.get("@type") == "TechArticle"]
        crumbs = [n for n in nodes if n.get("@type") == "BreadcrumbList"]
        if len(articles) != 1 or len(crumbs) != 1:
            fail(f"{name}: expected exactly one TechArticle and one "
                 f"BreadcrumbList, found {len(articles)}/{len(crumbs)} (§6.2)")
            continue
        a, c = articles[0], crumbs[0]
        # [2][3] identity and refs
        if a.get("@id") != canonical + "#article" or a.get("url") != canonical:
            fail(f"{name}: TechArticle @id/url mismatch the canonical URL (§6.2)")
        for field, atid in (("isPartOf", f"{SITE_URL}/#website"),
                            ("about", f"{SITE_URL}/#software"),
                            ("publisher", f"{SITE_URL}/#organization")):
            if a.get(field, {}).get("@id") != atid:
                fail(f"{name}: {field} must reference {atid} (§6.3)")
        # [4] no entity re-definitions
        if any(n.get("@type") in ("Organization", "WebSite",
                                  "SoftwareSourceCode") for n in nodes):
            fail(f"{name}: re-defines a §2 entity (§6.4)")
        # [5] dateModified shape
        if not DATE_RE.match(a.get("dateModified", "")):
            fail(f"{name}: dateModified '{a.get('dateModified')}' is not a date (§6.5)")
        if not a.get("headline") or not a.get("description"):
            fail(f"{name}: headline/description are required")
        # [8] inLanguage
        if a.get("inLanguage") != loc:
            fail(f"{name}: inLanguage '{a.get('inLanguage')}' != content locale '{loc}' (§6.8)")
        # breadcrumb shape
        items = c.get("itemListElement", [])
        if not items or items[0].get("item") != SITE_URL + "/":
            fail(f"{name}: breadcrumb must start at the site root")
        if items and items[-1].get("item") != canonical:
            fail(f"{name}: breadcrumb last item must be the page itself")
        # [6] pairs + [7] triple consistency
        en_only = rel.startswith("en/reference/api/") or \
            rel == "en/reference/changelog.html"
        twin = ("ru/" + rel.split("/", 1)[1]) if loc == "en" else \
            ("en/" + rel.split("/", 1)[1])
        if en_only:
            if "workTranslation" in a or "translationOfWork" in a:
                fail(f"{name}: EN-only page must not carry translation fields")
        else:
            field = "workTranslation" if loc == "ru" else "translationOfWork"
            want = f"{DOCS_URL}/{twin}#article"
            if a.get(field, {}).get("@id") != want:
                fail(f"{name}: {field} must be {want} (§6.7)")
            if not (SITE / twin).is_file():
                fail(f"{name}: twin page {twin} is missing (§6.6)")
        # head links
        text = html.read_text(encoding="utf-8")
        head = re.search(r"<head>(.*?)</head>", text, re.S)
        links = dict()
        if head:
            for m in re.finditer(
                    r'<link rel="alternate" hreflang="([^"]+)" href="([^"]+)"',
                    head.group(1)):
                links[m.group(1)] = m.group(2)
        else:
            fail(f"{name}: no <head> found")
        # [7] head == sitemap == ld+json
        sm = sitemap.get(canonical)
        if sm is None:
            fail(f"{name}: missing from the sitemap (§6.1)")
        else:
            if links != sm:
                fail(f"{name}: head hreflang links differ from sitemap annotations (§6.7)")
        expected = {loc: canonical}
        if not en_only:
            expected[("ru" if loc == "en" else "en")] = f"{DOCS_URL}/{twin}"
        expected["x-default"] = f"{DOCS_URL}/index.html"
        if links != expected:
            fail(f"{name}: head links {links} != expected {expected} (§6.7)")


def main():
    if not SITE.exists():
        print(f"VERIFY FAIL: {SITE} not found - run `make docsite` first")
        return 1
    pages = content_pages()
    sitemap = check_sitemap(pages)
    check_pages(pages, sitemap)
    total = sum(1 for _ in SITE.rglob("*") if _.is_file())
    if errors:
        print(f"VERIFY FAIL: {len(errors)} violation(s):")
        for e in errors[:30]:
            print(f"  - {e}")
        return 1
    print(f"  verify: contract checklist ok - {len(pages)} content pages, "
          f"{len(sitemap)} sitemap URLs, {total} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
