"""Hooks for the unified libmdbx docsite (docs/docsite/{en,ru}/mkdocs.yml).

Each locale build uses the matching locale subtree as docs_dir
(docs/{ru,en}), so a page URL is relative to the locale root and the page's
twin in the other locale has the same path. This hook prepends the language
pill (EN <-> RU) linking to that twin. Pages that exist only in the EN tree
(the generated API reference and the change log) link to the RU reference
landing instead - the RU landing explains that the reference is English-only.
"""

LOCALES = ("en", "ru")

# mkdoxy generates doxygen-style links inside its pages that mkdocs cannot
# resolve or that point to anchors mkdoxy itself never renders; clean them up
# so `mkdocs build --strict` passes. Applied to generated API pages only.
import re

_MKDOXY_LINK_FIXES = (
    # doxygen page refs -> the mkdoxy-generated page files
    (re.compile(r"\]\(intro\.html"), "](intro.md"),
    (re.compile(r"\]\(usage\.html"), "](usage.md"),
    # the related-pages listing links the main doxygen page by its refid
    (re.compile(r"\]\(indexpage\.md"), "](index.md"),
    # anchors mkdoxy links to but never emits (unnamed enums, operators,
    # friends): fall back to the page-level link
    (re.compile(r"\]\(([^)]+?)#(?:enum-@\w+|function-operator-[^)#]+|friend-[^)#]+)\)"),
     r"](\1)"),
)


def _clean_mkdoxy_markdown(markdown: str) -> str:
    for pattern, repl in _MKDOXY_LINK_FIXES:
        markdown = pattern.sub(repl, markdown)
    return markdown


def on_page_markdown(markdown, *, page, config, **_kwargs):
    if page.file.src_uri.startswith("reference/api/"):
        markdown = _clean_mkdoxy_markdown(markdown)
    return markdown


def _other(locale: str) -> str:
    return "ru" if locale == "en" else "en"


def _twin_url(url: str, locale: str, other: str) -> str:
    """Twin page path relative to the docsite root (the parent of the locale
    trees): the same path under the other locale; EN-only reference pages
    fall back to the RU reference landing."""
    if url.startswith("reference/api/") or url == "reference/changelog.html":
        return f"{other}/reference/index.html"
    return f"{other}/{url}"


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
    return pill + html
