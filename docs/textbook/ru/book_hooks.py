"""Build-info hooks for the Russian libmdbx textbook.

Prepends a banner with the library version (from `git describe`, passed by
the `books` make target) and the build date to every page, plus links to
the online version and the PDF that ships next to the HTML. Works for both
the HTML site and the PDF (the banner is rendered by the PDF engine too).
"""

import os

BANNER = (
    '<div class="book-build-info" '
    'style="margin:0 0 1.5em;padding:.4em 1em;font-size:.72em;opacity:.85;'
    'border:1px solid currentColor;border-radius:.3em;">'
    "libmdbx {version} — собрано {date} · "
    '<a href="{site_url}">онлайн-версия</a> · '
    '<a href="{pdf_url}">PDF-версия</a>'
    "</div>"
)


def on_config(config):
    """Assemble the cover build stamp before the PDF plugin builds its
    template keywords (mkdocs calls hooks' on_config before the cover is
    rendered, and the plugin picks keywords from `config.extra`), and point
    the with-pdf plugin to our custom cover template (the plugin resolves
    custom_template_path against the process CWD, so an absolute path is
    required for builds started from the repository root)."""
    version = os.environ.get("MDBX_BOOK_VERSION", "unknown")
    date = os.environ.get("MDBX_BOOK_DATE", "unknown")
    config["extra"]["build_stamp"] = f"SkyNET@K-PAX {date} {version}"
    plugin = config.get("plugins", {}).get("with-pdf")
    if plugin is not None:
        plugin._options.custom_template_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "templates")
    return config


def on_page_content(html, page, config, files):
    version = os.environ.get("MDBX_BOOK_VERSION", "unknown")
    date = os.environ.get("MDBX_BOOK_DATE", "unknown")
    site_url = config.get("site_url") or ""
    pdf_url = site_url + "libmdbx-textbook-ru.pdf"
    banner = BANNER.format(version=version, date=date, site_url=site_url, pdf_url=pdf_url)
    return banner + html
