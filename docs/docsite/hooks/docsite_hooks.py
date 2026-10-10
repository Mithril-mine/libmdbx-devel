"""Hooks for the unified libmdbx docsite (docs/docsite/{en,ru}/mkdocs.yml).

1. on_files:        re-map the locale landing page (config.extra.index_source,
                    e.g. `docsite/en/index.md`) to the site root as index.md,
                    so the deployed locale root resolves to a real index.html
                    on S3 static hosting;
2. on_page_content: prepend the language pill (EN <-> RU) linking to the twin
                    page in the other locale. Twin pages have symmetric URLs
                    (guides/overview.en.html <-> guides/overview.ru.html);
                    API pages (mkdoxy-generated, English-only) and the landing
                    page link to the other locale's root.
"""

from pathlib import Path

from mkdocs.structure.files import File

LOCALES = ("en", "ru")


def _other(locale: str) -> str:
    return "ru" if locale == "en" else "en"


def on_files(files, *, config):
    index_source = config.extra.get("index_source")
    if not index_source:
        return files
    for file in list(files):
        if file.src_uri == index_source:
            files.remove(file)
            files.append(
                File(
                    "index.md",
                    str(Path(file.abs_src_path).parent),
                    config.site_dir,
                    config.use_directory_urls,
                )
            )
            break
    return files


def _twin_url(url: str, locale: str, other: str) -> str:
    """Twin page path relative to the site root: guides/engineering/API-landing
    pages use the .{locale}.html suffix, textbook pages live under
    textbook/{locale}/, mkdoxy API pages and the landing page fall back to the
    other locale root."""
    if url == "index.html":
        return f"{other}/index.html"
    if url.startswith("api/"):
        return f"{other}/index.html"
    if url.startswith("textbook/"):
        parts = url.split("/", 2)
        if len(parts) == 3 and parts[1] in LOCALES:
            return f"{other}/textbook/{other}/{parts[2]}"
    if url.endswith(f".{locale}.html"):
        # twin pages have symmetric paths with the locale suffix swapped
        return f"{other}/{url[: -len(f'.{locale}.html')]}.{other}.html"
    return f"{other}/{url}"


def on_page_content(html, *, page, config, **_kwargs):
    locale = config.extra.get("locale", "en")
    other = _other(locale)
    url = page.url  # flat .html pages (use_directory_urls: false)
    twin = _twin_url(url, locale, other)
    # the twin path is site-root relative: prepend one '../' per level of
    # depth of this page (its own dir counts too)
    prefix = "../" * (url.count("/") + 1)
    pill = (
        f'<div class="lang-pill" '
        'style="float:right;margin:0 0 .5em 1em;padding:.1em .9em;'
        'font-size:.72em;border:1px solid currentColor;border-radius:1em;">'
        f'<a href="{prefix}{twin}">{other.upper()}</a></div>'
    )
    return pill + html
