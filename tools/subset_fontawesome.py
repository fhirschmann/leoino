#!/usr/bin/env python3
"""Subset the vendored Font Awesome CSS and WOFF2 files to icons used by the UI.

The firmware serves every web asset from flash, so shipping Font Awesome's complete
icon catalogue wastes both flash and cold-load bandwidth. This script discovers icon
classes in the application HTML/JavaScript, keeps their CSS mappings, and subsets all
three icon fonts to the corresponding Unicode codepoints.

Requires ``fonttools`` and ``brotli`` (for example in a temporary virtualenv):

    python -m pip install fonttools brotli
    python tools/subset_fontawesome.py
    python tools/bundle_assets.py

The committed CSS/fonts are already subsetted. When adding an icon that is absent from
them, pass pristine Font Awesome inputs with ``--source-css`` and ``--source-font-dir``.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from fontTools import subset
    from fontTools.ttLib import TTFont
except ImportError as exc:
    raise SystemExit("Install the script dependencies with: pip install fonttools brotli") from exc


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CSS = ROOT / "html" / "css" / "all.min.css"
DEFAULT_FONT_DIR = ROOT / "html" / "webfonts"

FONT_FILES = {
    "solid": "fa-solid-900.woff2",
    "regular": "fa-regular-400.woff2",
    "brands": "fa-brands-400.woff2",
}
STYLE_CLASSES = {
    "fa": "solid",
    "fas": "solid",
    "fa-solid": "solid",
    "far": "regular",
    "fa-regular": "regular",
    "fab": "brands",
    "fa-brands": "brands",
}

ICON_RE = re.compile(r"(?<![a-z0-9-])(fa-[a-z0-9-]+)")
CLASS_RE = re.compile(r"\bclass(?:Name)?\s*=\s*([\"'])(.*?)\1", re.DOTALL)
RULE_RE = re.compile(r"([^{}]+)\{([^{}]*)\}")
CODEPOINT_RE = re.compile(r'^content:"\\([0-9a-fA-F]{1,6})"$')
UTILITY_RE = re.compile(
    r"^fa-(?:[0-9]+x|xs|sm|lg|xl|2xl|fw|ul|li|border|pull-left|pull-right|"
    r"spin|spin-pulse|pulse|beat|bounce|fade|beat-fade|flip|shake|inverse|"
    r"rotate-[0-9]+|flip-(?:horizontal|vertical|both)|stack(?:-[12]x)?)$"
)


def application_sources() -> list[Path]:
    sources = sorted((ROOT / "html").glob("*.html"))
    sources.extend(
        path
        for path in sorted((ROOT / "html" / "js").glob("*.js"))
        if ".min." not in path.name and path.name != "vendor.min.js"
    )
    sources.extend(sorted((ROOT / "tools" / "demo").glob("*.js")))
    return sources


def discover_icons() -> tuple[set[str], dict[str, set[str]]]:
    icons: set[str] = set()
    styled: dict[str, set[str]] = {family: set() for family in FONT_FILES}

    for path in application_sources():
        text = path.read_text(encoding="utf-8")
        for icon in ICON_RE.findall(text):
            if not UTILITY_RE.fullmatch(icon) and not icon.endswith("-"):
                icons.add(icon)

        # Validate literal class attributes against the correct source font. Icons
        # assembled dynamically are still retained in every font in which they exist.
        for match in CLASS_RE.finditer(text):
            classes = set(match.group(2).split())
            families = {STYLE_CLASSES[item] for item in classes if item in STYLE_CLASSES}
            class_icons = {
                item
                for item in classes
                if ICON_RE.fullmatch(item) and not UTILITY_RE.fullmatch(item)
            }
            for family in families:
                styled[family].update(class_icons)

    return icons, styled


def css_codepoints(css: str) -> dict[str, int]:
    mapping: dict[str, int] = {}
    for selector, body in RULE_RE.findall(css):
        codepoint_match = CODEPOINT_RE.fullmatch(body)
        if not codepoint_match:
            continue
        codepoint = int(codepoint_match.group(1), 16)
        for icon in ICON_RE.findall(selector):
            mapping[icon] = codepoint
    return mapping


def trim_css(css: str, icons: set[str]) -> str:
    def keep_used_rule(match: re.Match[str]) -> str:
        codepoint_match = CODEPOINT_RE.fullmatch(match.group(2))
        if codepoint_match and not (set(ICON_RE.findall(match.group(1))) & icons):
            return ""
        return match.group(0)

    return RULE_RE.sub(keep_used_rule, css)


def load_font_codepoints(font_dir: Path) -> dict[str, set[int]]:
    available: dict[str, set[int]] = {}
    for family, filename in FONT_FILES.items():
        with TTFont(font_dir / filename, lazy=True) as font:
            available[family] = set(font.getBestCmap())
    return available


def validate(
    icons: set[str],
    styled: dict[str, set[str]],
    mapping: dict[str, int],
    available: dict[str, set[int]],
) -> set[int]:
    missing_css = sorted(icons - mapping.keys())
    if missing_css:
        raise ValueError("Font Awesome CSS has no mapping for: " + ", ".join(missing_css))

    for family, family_icons in styled.items():
        missing_font = sorted(
            icon for icon in family_icons if mapping[icon] not in available[family]
        )
        if missing_font:
            raise ValueError(
                f"{FONT_FILES[family]} lacks icons used with its style: "
                + ", ".join(missing_font)
            )

    missing_all = sorted(
        icon
        for icon in icons
        if not any(mapping[icon] in codepoints for codepoints in available.values())
    )
    if missing_all:
        raise ValueError("No source font contains: " + ", ".join(missing_all))

    return {mapping[icon] for icon in icons}


def subset_fonts(source_dir: Path, output_dir: Path, codepoints: set[int]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for filename in FONT_FILES.values():
        source = source_dir / filename
        target = output_dir / filename
        before = source.stat().st_size

        font = TTFont(source)
        options = subset.Options()
        options.flavor = "woff2"
        options.recalc_timestamp = False
        options.canonical_order = True
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=codepoints)
        subsetter.subset(font)

        temporary = target.with_name(target.name + ".tmp")
        font.save(temporary)
        temporary.replace(target)
        print(f"  {filename}: {before:,} -> {target.stat().st_size:,} bytes")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-css", type=Path, default=DEFAULT_CSS)
    parser.add_argument("--source-font-dir", type=Path, default=DEFAULT_FONT_DIR)
    parser.add_argument("--output-css", type=Path, default=DEFAULT_CSS)
    parser.add_argument("--output-font-dir", type=Path, default=DEFAULT_FONT_DIR)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    css = args.source_css.read_text(encoding="utf-8")
    icons, styled = discover_icons()
    mapping = css_codepoints(css)
    available = load_font_codepoints(args.source_font_dir)
    codepoints = validate(icons, styled, mapping, available)

    trimmed = trim_css(css, icons)
    args.output_css.parent.mkdir(parents=True, exist_ok=True)
    args.output_css.write_text(trimmed, encoding="utf-8")
    print(f"Font Awesome: retaining {len(icons)} icon classes / {len(codepoints)} glyphs")
    print(f"  all.min.css: {len(css):,} -> {len(trimmed):,} bytes")
    subset_fonts(args.source_font_dir, args.output_font_dir, codepoints)
    print("Run tools/bundle_assets.py to refresh vendor.min.css.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
