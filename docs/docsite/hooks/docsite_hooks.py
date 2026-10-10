"""Hooks for the unified libmdbx docsite (docs/docsite/{en,ru}/mkdocs.yml).

Each locale build uses the matching locale subtree as docs_dir
(docs/{ru,en}), so a page URL is relative to the locale root and the page's
twin in the other locale has the same path. This hook provides:

1. on_page_markdown - cleanup of mkdoxy-generated links so `--strict` passes;
2. on_page_context  - collects the git last-modified date per page into a
   manifest consumed by normalize_site.py (the sitemap's <lastmod>);
3. on_page_content  - the language pill (EN <-> RU) plus the JSON-LD block
   (TechArticle + BreadcrumbList, with the mandatory RU<->EN translation
   linking) per the docs-integration-contract §4;
4. on_post_page     - hreflang <link> annotations in <head> (§4.3), kept
   consistent with the sitemap (§3.1) and the ld+json translation fields
   (§4.1) by construction: all three derive from the same twin functions.

Pages that exist only in the EN tree (the generated API reference and the
change log) link to the RU reference landing instead - the RU landing
explains that the reference is English-only - and carry no translation
fields (fixed by the contract amendment).
"""

import datetime
import json
import re
import subprocess

LOCALES = ("en", "ru")

# canonical origin and the /docs/ base from docs-integration-contract §2
SITE_URL = "https://libmdbx.dqdkfa.ru"
DOCS_URL = SITE_URL + "/docs"

# taxonomy per docs-integration-contract §4.2, names in the page language
SECTIONS = {
    "ru": {
        "overview": "Обзор",
        "getting-started": "Начало работы",
        "textbook": "Учебник",
        "deep-dive": "Погружение",
        "reference": "Справочник",
        "home": "Главная",
    },
    "en": {
        "overview": "Overview",
        "getting-started": "Getting started",
        "textbook": "Textbook",
        "deep-dive": "Deep dive",
        "reference": "Reference",
        "home": "Home",
    },
}

# the canonical entry page of each section (the breadcrumb level-2 item)
SECTION_ENTRY = {
    "overview": "overview/index.html",
    "getting-started": "getting-started/install-build.html",
    "textbook": "textbook/index.html",
    "deep-dive": "deep-dive/architecture.html",
    "reference": "reference/index.html",
}

# mkdoxy generates doxygen-style links inside its pages that mkdocs cannot
# resolve or that point to anchors mkdoxy itself never renders; clean them up
# so `mkdocs build --strict` passes. Applied to generated API pages only.
_MKDOXY_LINK_FIXES = (
    # doxygen page refs -> the mkdoxy-generated page files
    (re.compile(r"\]\(intro\.html"), "](intro.md"),
    (re.compile(r"\]\(usage\.html"), "](usage.md"),
    # the related-pages listing links the main doxygen page by its refid
    (re.compile(r"\]\(indexpage\.md"), "](index.md"),
    # anchors mkdoxy links to but never emits (unnamed enums, operators,
    # friends - same-page and cross-page): fall back to the page-level link
    (re.compile(r"\]\(([^)]*?)#(?:enum-@\w+|function-operator-[^)#]+|friend-[^)#]+)\)"),
     r"](\1)"),
    # doxygen \anchor targets are rendered by mkdoxy with a section suffix
    # (long-lived-read -> long-lived-read-transactions): the bare anchor does
    # not exist, link to the page instead
    (re.compile(r"\]\(([^)]*?)#long-lived-read\)"), r"](\1)"),
    # the license blurb links ./LICENSE which mkdocs cannot resolve; point it
    # at the canonical shipped copy (normalize_site.py localizes it back to a
    # relative path)
    (re.compile(r"\]\(\.?/?LICENSE\)"),
     "](https://libmdbx.dqdkfa.ru/docs/en/LICENSE)"),
)

# pages present only in the EN tree: no RU pair -> no translation fields,
# hreflang carries en + x-default only
def _en_only(url: str) -> bool:
    return url.startswith("reference/api/") or url == "reference/changelog.html"


def _other(locale: str) -> str:
    return "ru" if locale == "en" else "en"


def _canonical(locale: str, url: str) -> str:
    """Canonical URL of a page; `url` is relative to the locale root."""
    if url.startswith(tuple(f"{l}/" for l in LOCALES)):
        return f"{DOCS_URL}/{url}"
    return f"{DOCS_URL}/{locale}/{url}"


def _twin_url(url: str, locale: str, other: str) -> str:
    """Twin page path relative to the docsite root (the parent of the locale
    trees): the same path under the other locale; EN-only reference pages
    fall back to the RU reference landing."""
    if _en_only(url):
        return f"{other}/reference/index.html"
    return f"{other}/{url}"


def _clean_mkdoxy_markdown(markdown: str) -> str:
    for pattern, repl in _MKDOXY_LINK_FIXES:
        markdown = pattern.sub(repl, markdown)
    return markdown


def on_page_markdown(markdown, *, page, config, **_kwargs):
    if page.file.src_uri.startswith("reference/api/"):
        markdown = _clean_mkdoxy_markdown(markdown)
    return markdown


# ---------------------------------------------------------------------------
# lastmod manifest (consumed by normalize_site.py for the sitemap <lastmod>)

_LASTMOD = {}
_MANIFEST = ".lastmod.json"


def _git_date(config, src_path) -> str:
    if not isinstance(src_path, __import__("pathlib").Path):
        src_path = __import__("pathlib").Path(src_path)
    """Git last-commit date (YYYY-MM-DD) of a source file; falls back to the
    file mtime and, for generated/untracked content, to the date of its
    upstream source."""
    repo = None
    try:
        repo = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True,
            text=True, check=True).stdout.strip()
    except Exception:
        repo = None
    candidates = [str(src_path)]
    name = str(src_path).rsplit("/", 1)[-1]
    if name == "changelog.md":
        # generated copy of the root ChangeLog.md
        candidates.append(str(src_path).replace(
            "/docs/en/reference/changelog.md", "/ChangeLog.md"))
    elif src_path.parts[-3:-1] == ("assets", ".doxy") or \
            "/reference/api/" in str(src_path):
        # mkdoxy work area: the API reference derives from the doxygen inputs
        base = str(config.docs_dir).rsplit("/docs/", 1)[0]
        candidates = [f"{base}/docs/mdbx.h", f"{base}/docs/mdbx.h++",
                      f"{base}/docs/options.h"]
    for path in candidates:
        if repo and not path.startswith("/"):
            path = f"{repo}/{path}"
        try:
            out = subprocess.run(
                ["git", "log", "-1", "--format=%cs", "--", path],
                capture_output=True, text=True, check=True, cwd=repo).stdout.strip()
            if out:
                return out
        except Exception:
            pass
    try:
        stamp = datetime.datetime.fromtimestamp(
            __import__("os").path.getmtime(str(src_path)))
        return stamp.strftime("%Y-%m-%d")
    except Exception:
        return datetime.date.today().strftime("%Y-%m-%d")


def on_post_build(*, config, **_kwargs):
    """Merge this build's dates into the shared manifest at the docsite root
    (site_dir is the per-locale tree; the docsite root is its parent and both
    locale builds write into the same file, sequentially)."""
    path = config.site_dir + "/../" + _MANIFEST
    try:
        merged = dict(json.load(open(path, encoding="utf-8")))
    except Exception:
        merged = {}
    merged.update(_LASTMOD)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(merged, f, ensure_ascii=False, indent=1, sort_keys=True)


# ---------------------------------------------------------------------------
# language pill + JSON-LD (contract §4)

def _description(html: str) -> str:
    """Article description (1-2 sentences): the first substantial paragraph;
    header cross-link blocks are skipped when a longer one follows."""
    best = ""
    for m in re.finditer(r"<p>(.*?)</p>", html, re.S):
        text = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", m.group(1))).strip()
        if not text:
            continue
        # prose outside of <a> elements: link-only blocks (navigation "see
        # also" rows) do not describe the article
        prose = re.sub(r"\s+", " ",
                       re.sub(r"<a[^>]*>.*?</a>", "", m.group(1),
                              flags=re.S)).strip()
        if len(prose) >= 60:
            return text[:200]
        if not best and len(text) >= 40:
            best = text[:200]
    return best


def _breadcrumb(url: str, locale: str, title: str) -> dict:
    names = SECTIONS[locale]
    items = [{"@type": "ListItem", "position": 1, "name": names["home"],
              "item": SITE_URL + "/"}]
    section = url.split("/", 1)[0]
    entry = SECTION_ENTRY.get(section)
    if entry:
        items.append({"@type": "ListItem", "position": 2,
                      "name": names.get(section, section),
                      "item": _canonical(locale, entry)})
    items.append({"@type": "ListItem", "position": len(items) + 1,
                  "name": title, "item": _canonical(locale, url)})
    return {"@type": "BreadcrumbList",
            "@id": _canonical(locale, url) + "#breadcrumb",
            "itemListElement": items}


def on_page_content(html, *, page, config, **_kwargs):
    locale = config.extra.get("locale", "en")
    other = _other(locale)
    url = page.url  # flat .html pages (use_directory_urls: false)

    twin = _twin_url(url, locale, other)
    # the twin path is docsite-root relative: this page sits `count`
    # directories below its locale root, the locale root sits one level
    # below the docsite root
    prefix = "../" * (url.count("/") + 1)
    pill = (
        f'<div class="lang-pill" '
        'style="float:right;margin:0 0 .5em 1em;padding:.1em .9em;'
        'font-size:.72em;border:1px solid currentColor;border-radius:1em;">'
        f'<a href="{prefix}{twin}">{other.upper()}</a></div>'
    )

    if url != "404.html" and url not in _LASTMOD:
        # page_content fires before page_context in mkdocs, so the date is
        # collected here for both the article and the sitemap manifest
        _LASTMOD[url] = _git_date(config, page.file.abs_src_path)
    article = {
        "@type": "TechArticle",
        "@id": _canonical(locale, url) + "#article",
        "url": _canonical(locale, url),
        "headline": page.title or url,
        # generated index/listing pages may carry no prose paragraph
        "description": _description(html)
        or f"{page.title or url} - libmdbx documentation.",
        "inLanguage": locale,
        "dateModified": _LASTMOD.get(url, ""),
        "isPartOf": {"@id": SITE_URL + "/#website"},
        "about": {"@id": SITE_URL + "/#software"},
        "publisher": {"@id": SITE_URL + "/#organization"},
    }
    if not _en_only(url):
        # RU is the canonical original (workTranslation), EN the translation
        # (translationOfWork) - docs-integration-contract §4.1
        twin_article = _canonical(other, _twin_url(url, locale, other)) + "#article"
        article["workTranslation" if locale == "ru" else "translationOfWork"] = \
            {"@id": twin_article}
    graph = [article, _breadcrumb(url, locale, page.title or url)]

    ld = ('<script type="application/ld+json">'
          + json.dumps({"@context": "https://schema.org", "@graph": graph},
                       ensure_ascii=False)
          + "</script>")
    return pill + html + ld


# ---------------------------------------------------------------------------
# hreflang <head> links (contract §4.3) - must match the sitemap §3.1

# Yandex.Metrika counter, canonical snippet (owner-provided), id 99261645.
# Injected as a whole right after <body> - the same placement the legacy
# doxygen header used. Independent of docs/header.html.
_METRIKA_BLOCK = (
    "<!-- Яндекс Метрика -->\n"
    '<script type="text/javascript" >\n'
    "   (function(m,e,t,r,i,k,a){m[i]=m[i]||function(){(m[i].a=m[i].a||[]).push(arguments)};\n"
    "   m[i].l=1*new Date();k=e.createElement(t),a=e.getElementsByTagName(t)[0],"
    "k.async=1,k.src=r,a.parentNode.insertBefore(k,a)})\n"
    '   (window, document, "script", "https://mc.yandex.ru/metrika/tag.js", "ym");\n'
    "\n"
    '   ym(99261645, "init", {\n'
    "       clickmap:true,\n"
    "       trackLinks:true,\n"
    "       accurateTrackBounce:true,\n"
    "       webvisor:true\n"
    "   });\n"
    "</script>\n"
    '<noscript><div><img src="https://mc.yandex.ru/watch/99261645" '
    'style="position:absolute; left:-9999px;" alt="" /></div></noscript>\n'
    "<!-- /Яндекс Метрика -->"
)

# Brand block, verbatim from the landing page head (favicon set, shared PWA
# manifest, application name) - the icons live on the app side at
# /static-content/ and are referenced, not copied.
_BRAND_HEAD = (
    '<link rel="icon" href="/static-content/icon.svg" type="image/svg+xml"/>'
    '<link rel="icon" href="/static-content/favicon-32x32.png" '
    'type="image/png" sizes="32x32"/>'
    '<link rel="icon" href="/static-content/favicon-16x16.png" '
    'type="image/png" sizes="16x16"/>'
    '<link rel="icon" href="/static-content/icon-192.png" '
    'type="image/png" sizes="192x192"/>'
    '<link rel="apple-touch-icon" href="/static-content/apple-touch-icon.png" '
    'sizes="180x180" type="image/png"/>'
    '<link rel="manifest" href="/static-content/manifest.webmanifest"/>'
    '<meta name="application-name" content="libmdbx"/>'
)


def on_post_page(output, *, page, config, **_kwargs):
    # brand links into <head> (the Material favicon link itself comes from
    # theme.favicon); hreflang links below must stay absolute (contract §4.3)
    output = output.replace("</head>", _BRAND_HEAD + "\n</head>", 1)
    # analytics counter: the whole block (script + noscript) right after the
    # body open tag, as on the legacy doxygen pages
    output = re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + "\n" + _METRIKA_BLOCK,
                    output, count=1)
    locale = config.extra.get("locale", "en")
    other = _other(locale)
    url = page.url
    if url == "404.html":
        return output
    links = [f'<link rel="alternate" hreflang="{locale}" '
             f'href="{_canonical(locale, url)}"/>']
    if not _en_only(url):
        links.append(
            f'<link rel="alternate" hreflang="{other}" '
            f'href="{_canonical(other, _twin_url(url, locale, other))}"/>')
    links.append(f'<link rel="alternate" hreflang="x-default" '
                 f'href="{DOCS_URL}/index.html"/>')
    return output.replace("</head>", "\n".join(links) + "\n</head>", 1)
