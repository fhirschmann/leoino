#!/usr/bin/env python3
"""Concatenate the vendored web libraries into two bundles so the management
page only needs two requests (one JS, one CSS) instead of ~15. This keeps the
UI fully offline-capable while avoiding the ESPAsyncWebServer connection-limit
stalls that some browsers (Safari/Chrome) hit when many assets are requested
in parallel.

Run from the repo root after updating any vendored lib:  python3 tools/bundle_assets.py
Outputs html/js/vendor.min.js and html/css/vendor.min.css (committed artifacts).
"""
from pathlib import Path

HTML = Path(__file__).resolve().parent.parent / "html"

# order matters: jQuery before its plugins, i18next before loc-i18next
JS = [
    "js/jquery.min.js",
    "js/jquery-ui.min.js",
    "js/bootstrap.bundle.min.js",
    "js/jstree.min.js",
    "js/bootstrap-slider.min.js",
    "js/i18next.min.js",
    "js/i18nextHttpBackend.min.js",
    "js/loc_i18next.min.js",
    "js/natcompare.js",
]

# (file, [(old, new), ...]) — rewrite relative url() to absolute device paths
CSS = [
    ("css/bootstrap.min.css", []),
    ("jstree/default/style.min.css", [
        ('url("32px.png")', 'url("/jstree/default/32px.png")'),
        ('url("40px.png")', 'url("/jstree/default/40px.png")'),
        ('url("throbber.gif")', 'url("/jstree/default/throbber.gif")'),
    ]),
    ("jstree/default-dark/style.min.css", [
        ('url("32px.png")', 'url("/jstree/default-dark/32px.png")'),
        ('url("40px.png")', 'url("/jstree/default-dark/40px.png")'),
        ('url("throbber.gif")', 'url("/jstree/default-dark/throbber.gif")'),
    ]),
    ("css/all.min.css", [("../webfonts/", "/webfonts/")]),
    ("css/bootstrap-slider.min.css", []),
    ("fonts/fonts.css", []),  # already uses absolute /fonts/ paths
]

def build_js():
    parts = []
    for rel in JS:
        parts.append(f"/* {rel} */")
        parts.append((HTML / rel).read_text(encoding="utf-8"))
    # ';' between files guards against ASI issues when concatenating minified IIFEs
    out = "\n;\n".join(parts) + "\n"
    (HTML / "js/vendor.min.js").write_text(out, encoding="utf-8")
    print(f"js/vendor.min.js  ({len(out)} bytes from {len(JS)} files)")

def build_css():
    parts = []
    for rel, repls in CSS:
        css = (HTML / rel).read_text(encoding="utf-8")
        for old, new in repls:
            assert old in css, f"expected {old!r} in {rel}"
            css = css.replace(old, new)
        parts.append(f"/* {rel} */")
        parts.append(css)
    out = "\n".join(parts) + "\n"
    (HTML / "css/vendor.min.css").write_text(out, encoding="utf-8")
    print(f"css/vendor.min.css ({len(out)} bytes from {len(CSS)} files)")

if __name__ == "__main__":
    build_js()
    build_css()
