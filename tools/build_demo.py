#!/usr/bin/env python3
"""Build a self-contained ESPuino Web-UI demo for GitHub Pages.

Takes the regular firmware UI in ``html/`` and turns it into a static
site that needs no ESP32: a mock layer (``tools/demo/demo-mock.js``)
fakes the WebSocket and every REST endpoint. The result is written to an
output directory (default ``demo_dist/``) that can be published as the
Pages artifact.

Transformations applied to ``management.html`` -> ``index.html``:
  * absolute asset paths (``/css``, ``/js``, ``/logo.svg`` ...) are made
    document-relative so the site works from a project sub-path
    (``user.github.io/leoino/``);
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
DEMO = ROOT / "tools" / "demo"

# Static asset trees/files copied verbatim from html/ into the demo.
COPY_ITEMS = [
    "css", "js", "fonts", "webfonts", "jstree", "locales",
    "logo.svg", "appicon.svg", "manifest.json",
]

# Extra demo-only assets.
DEMO_ASSETS = ["demo-mock.js", "demo-cover.svg", "demo.css"]


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
    }
    for a, b in replacements.items():
        out = out.replace(a, b)

    # 2) disable the service worker (it would cache device-only endpoints)
    out = out.replace("if ('serviceWorker' in navigator) {", "if (false) {")

    # 3) cover image -> bundled demo SVG (avoids http/mixed-content + 404)
    out = out.replace('"http://" + host + "/cover?"', '"demo-cover.svg?"')

    # 4) inject the mock loader as the very first <head> child, plus demo CSS
    head_inject = (
        '<head>\n'
        '\t<!-- ESPuino demo: mock layer must load before any app code -->\n'
        '\t<script src="demo-mock.js"></script>\n'
        '\t<link rel="stylesheet" href="demo.css">\n'
    )
    out = out.replace("<head>\n", head_inject, 1)

    # 5) demo banner right after <body>
    banner = (
        '\n\t<div id="demoBanner">\n'
        '\t\t<span class="demo-pill">DEMO</span>\n'
        '\t\t<span>Statische Vorschau des ESPuino-Webinterface &ndash; kein Geraet verbunden, Aktionen ohne Wirkung.</span>\n'
        '\t\t<a href="https://github.com/fhirschmann/leoino" target="_blank" rel="noopener">Projekt auf GitHub</a>\n'
        '\t</div>\n'
    )
    out = re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + banner, out, count=1)

    return out


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "demo_dist"
    out_dir = out_dir.resolve()

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

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

    # demo-only assets
    for item in DEMO_ASSETS:
        shutil.copy2(DEMO / item, out_dir / item)

    # transformed entry point
    html = (HTML / "management.html").read_text(encoding="utf-8")
    (out_dir / "index.html").write_text(transform_html(html), encoding="utf-8")

    # disable Jekyll so files/dirs starting with "_" are served untouched
    (out_dir / ".nojekyll").write_text("", encoding="utf-8")

    print(f"Demo built -> {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
