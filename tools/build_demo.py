#!/usr/bin/env python3
"""Build the GitHub-Pages site: landing page + self-contained Web-UI demo.

The published site has two parts:

  ``/``      the project landing page (``tools/demo/landing.html``)
  ``/demo/`` a device-free build of the management UI

The demo takes the regular firmware UI in ``html/`` and turns it into a
static site that needs no ESP32: a mock layer (``tools/demo/demo-mock.js``)
fakes the WebSocket and every REST endpoint. The result is written to an
output directory (default ``demo_dist/``) that can be published as the
Pages artifact.

Transformations applied to ``management.html`` -> ``index.html``:
  * absolute asset paths (``/css``, ``/js``, ``/logo.svg`` ...) are made
    document-relative so the site works from a project sub-path
    (``user.github.io/leoino/demo/``);
  * the service worker is disabled (it caches device endpoints);
  * the cover-image URL is pointed at the bundled demo SVG;
  * the mock script is injected first in <head>;
  * a "demo" banner is added.

Usage:  python3 tools/build_demo.py [output_dir]
"""
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT / "html"
DOCS_IMG = ROOT / "docs" / "img"
DEMO = ROOT / "tools" / "demo"

# Static asset trees/files copied verbatim from html/ into the demo.
COPY_ITEMS = [
    "css", "js", "fonts", "webfonts", "jstree", "locales",
    "logo.svg", "appicon.svg", "manifest.json",
]

# Extra demo-only assets.
DEMO_ASSETS = ["demo-mock.js", "demo-cover.svg", "demo-homekit-qr.svg", "demo.css"]

# Sub-directory of the published site the device-free UI demo lives in.
DEMO_SUBDIR = "demo"

# Fonts the landing page declares itself (@font-face with relative paths), copied
# out of html/fonts/ so the site stays free of any external request.
LANDING_FONTS = [
    "orbitron-500-latin-yMJRMIlzdpvBhQQL_Qq7dy0.woff2",
    "rajdhani-400-latin-LDIxapCSOBg7S-QT7p4HM-Y.woff2",
    "rajdhani-600-latin-LDI2apCSOBg7S-QT7pbYF_Oreec.woff2",
    "rajdhani-700-latin-LDI2apCSOBg7S-QT7pa8FvOreec.woff2",
    "sharetechmono-400-latin-J7aHnp1uDWRBEqV98dVQztYldFcLowEF.woff2",
]

# Screenshots/photos the landing page shows, copied from docs/img/ into img/.
LANDING_IMAGES = [
    "at1-finished-workbench.jpg", "at1-front.jpg", "at1-cartridge.jpg",
    "at1-rear-io.jpg", "at1-build-front-module.jpg",
    "at1-build-speaker-module.jpg", "at1-build-open-chassis.jpg",
    "at1-build-exploded.jpg", "at1-build-wiring.jpg", "at1-build-powered.jpg",
]


def transform_html(src: str) -> str:
    out = src

    # 1) make head asset references document-relative
    replacements = {
        'href="/manifest.json"': 'href="manifest.json"',
        'href="/logo.svg"': 'href="logo.svg"',
        'href="/appicon.svg"': 'href="appicon.svg"',
        'href="/favicon.ico"': 'href="favicon.ico"',
        'href="/css/vendor.min.css"': 'href="css/vendor.min.css"',
        'src="/js/vendor.min.js"': 'src="js/vendor.min.js"',
        'src="/logo"': 'src="logo.svg"',
        'src="/cover"': 'src="demo-cover.svg"',
    }
    for a, b in replacements.items():
        out = out.replace(a, b)

    # 2) disable the service worker (it would cache device-only endpoints)
    out = out.replace("if ('serviceWorker' in navigator) {", "if (false) {")

    # 3) cover image -> bundled demo SVG (avoids http/mixed-content + 404)
    #    two variants: the now-playing bar (double-quoted) and the track-info modal
    #    (concatenated into a double-quoted attribute: "http://' + host + '/cover?')
    out = out.replace('"http://" + host + "/cover?"', '"demo-cover.svg?"')
    out = out.replace('"http://\' + host + \'/cover?\'', '"demo-cover.svg?\'')

    # 3b) HomeKit pairing QR -> bundled placeholder SVG (same http/mixed-content + 404 reason)
    out = out.replace('"http://" + host + "/homekit/qr.svg?ts=" + Date.now()', '"demo-homekit-qr.svg?ts=" + Date.now()')

    # 4) inject the mock loader as the very first <head> child, plus demo CSS
    head_inject = (
        '<head>\n'
        '\t<!-- ESPuino demo: mock layer must load before any app code -->\n'
        '\t<script src="demo-mock.js"></script>\n'
        '\t<link rel="stylesheet" href="demo.css">\n'
    )
    out = out.replace("<head>\n", head_inject, 1)

    # 5) demo banner right after <body>. The data-i18n keys come from the "demo"
    #    namespace the mock merges into each locales file, so the banner follows
    #    the language selected in the UI. The inline text is the pre-i18n fallback.
    banner = (
        '\n\t<div id="demoBanner">\n'
        '\t\t<span class="demo-pill" data-i18n="demo.label">DEMO</span>\n'
        '\t\t<span data-i18n="demo.text">Statische Vorschau des ESPuino-Webinterface &ndash; kein Gerät verbunden, Aktionen ohne Wirkung.</span>\n'
        '\t\t<a href="../" data-i18n="demo.back">Zur Projektseite</a>\n'
        '\t\t<a href="https://github.com/fhirschmann/leoino" target="_blank" rel="noopener" data-i18n="demo.link">Projekt auf GitHub</a>\n'
        '\t</div>\n'
    )
    out = re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + banner, out, count=1)

    return out


def build_landing(out_dir: Path) -> None:
    """Write the project landing page and its assets into the site root."""
    shutil.copy2(HTML / "logo.svg", out_dir / "logo.svg")

    fonts_dir = out_dir / "fonts"
    fonts_dir.mkdir(exist_ok=True)
    for name in LANDING_FONTS:
        shutil.copy2(HTML / "fonts" / name, fonts_dir / name)

    img_dir = out_dir / "img"
    img_dir.mkdir(exist_ok=True)
    for name in LANDING_IMAGES:
        src = DOCS_IMG / name
        if not src.exists():
            print(f"  skip (missing image): {name}")
            continue
        shutil.copy2(src, img_dir / name)

    shutil.copy2(DEMO / "landing.html", out_dir / "index.html")


def build_demo(out_dir: Path) -> None:
    """Write the device-free management-UI demo into ``out_dir``."""
    out_dir.mkdir(parents=True, exist_ok=True)

    # copy static assets
    for item in COPY_ITEMS:
        src = HTML / item
        if not src.exists():
            print(f"  skip (missing): {item}")
            continue
        dst = out_dir / item
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)

    # The bundled CSS references assets from the server root (url(/webfonts/...),
    # url("/jstree/..."), ...) which 404 on a project sub-path such as
    # user.github.io/leoino/demo/. Every one of those trees is a sibling of css/,
    # so rewriting the leading slash to ../ makes icons, fonts and the jstree
    # sprites load. data: URIs keep their own scheme and are left alone.
    css_dir = out_dir / "css"
    if css_dir.is_dir():
        root_url = re.compile(r'url\((["\']?)/')
        for css in css_dir.glob("*.css"):
            text = css.read_text(encoding="utf-8")
            css.write_text(root_url.sub(r"url(\1../", text), encoding="utf-8")

    # demo-only assets
    for item in DEMO_ASSETS:
        shutil.copy2(DEMO / item, out_dir / item)

    # transformed entry point
    html = (HTML / "management.html").read_text(encoding="utf-8")
    (out_dir / "index.html").write_text(transform_html(html), encoding="utf-8")


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "demo_dist"
    out_dir = out_dir.resolve()

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    build_landing(out_dir)
    build_demo(out_dir / DEMO_SUBDIR)

    # disable Jekyll so files/dirs starting with "_" are served untouched
    (out_dir / ".nojekyll").write_text("", encoding="utf-8")

    print(f"Site built -> {out_dir}")
    print(f"  landing page : {out_dir / 'index.html'}")
    print(f"  Web-UI demo  : {out_dir / DEMO_SUBDIR / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
