# -*- coding: utf-8 -*-
from pathlib import Path
import os
import shutil
import mimetypes
import gzip
import json
import re

# Use PlatformIO's build directory to keep the project clean
# this matches your version 1 logic exactly
try:
    from SCons.Script import Import
    Import("env")
    OUTPUT_DIR = Path(env.subst("$BUILD_DIR")) / "generated"
    IS_PIO = True
except Exception:
    OUTPUT_DIR = Path("./generated")
    IS_PIO = False

# Ensure the directory exists so the script doesn't crash
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

# Ensure HTML_DIR is relative to the project root
ROOT_DIR = Path(env.subst("$PROJECT_DIR")) if IS_PIO else Path.cwd()
HTML_DIR = ROOT_DIR / "html"

BINARY_FILES = [
    Path("management.html"),
    Path("accesspoint.html"),
    Path("manifest.json"),
    Path("appicon.svg"),
    Path("sw.js"),
    Path("offline.html"),
    Path("logo.svg"),
    Path("js/i18next.min.js"),
    Path("js/i18nextHttpBackend.min.js"),
    Path("REST_API.yaml"),
    Path("swagger.html"),
    Path("js/swaggerInitializer.js"),
    Path("js/loc_i18next.min.js"),
    Path("locales/de.json"),
    Path("locales/en.json"),
    Path("locales/fr.json"),
    # vendored third-party libs (served locally so the UI works without internet)
    Path("js/jquery.min.js"),
    Path("js/jquery-ui.min.js"),
    Path("js/bootstrap.bundle.min.js"),
    Path("js/jstree.min.js"),
    Path("js/bootstrap-slider.min.js"),
    Path("js/natcompare.js"),
    Path("css/bootstrap.min.css"),
    Path("css/bootstrap-slider.min.css"),
    Path("css/all.min.css"),
    Path("webfonts/fa-solid-900.woff2"),
    Path("webfonts/fa-regular-400.woff2"),
    Path("webfonts/fa-brands-400.woff2"),
    Path("webfonts/fa-v4compatibility.woff2"),
    Path("jstree/default/style.min.css"),
    Path("jstree/default/32px.png"),
    Path("jstree/default/40px.png"),
    Path("jstree/default/throbber.gif"),
    Path("jstree/default-dark/style.min.css"),
    Path("jstree/default-dark/32px.png"),
    Path("jstree/default-dark/40px.png"),
    Path("jstree/default-dark/throbber.gif"),
    Path("fonts/fonts.css"),
    Path("fonts/orbitron-500-latin-yMJRMIlzdpvBhQQL_Qq7dy0.woff2"),
    Path("fonts/rajdhani-400-latin-LDIxapCSOBg7S-QT7p4HM-Y.woff2"),
    Path("fonts/rajdhani-400-latin-ext-LDIxapCSOBg7S-QT7p4JM-aUWA.woff2"),
    Path("fonts/rajdhani-500-latin-LDI2apCSOBg7S-QT7pb0EPOreec.woff2"),
    Path("fonts/rajdhani-500-latin-ext-LDI2apCSOBg7S-QT7pb0EPOleef2kg.woff2"),
    Path("fonts/rajdhani-600-latin-LDI2apCSOBg7S-QT7pbYF_Oreec.woff2"),
    Path("fonts/rajdhani-600-latin-ext-LDI2apCSOBg7S-QT7pbYF_Oleef2kg.woff2"),
    Path("fonts/rajdhani-700-latin-LDI2apCSOBg7S-QT7pa8FvOreec.woff2"),
    Path("fonts/rajdhani-700-latin-ext-LDI2apCSOBg7S-QT7pa8FvOleef2kg.woff2"),
    Path("fonts/sharetechmono-400-latin-J7aHnp1uDWRBEqV98dVQztYldFcLowEF.woff2")
]

class HtmlHeaderProcessor:
    @staticmethod
    def _safe_minify(content, suffix):
        lines = []
        for line in content.splitlines():
            stripped = line.strip()
            if stripped:
                if suffix in [".html", ".htm"]:
                    stripped = re.sub(r'>\s+<', '><', stripped)
                lines.append(stripped)
        return "\n".join(lines)

    @classmethod
    def _process_binary_file(cls, binary_path, header_path, info):
        if binary_path.suffix == ".json":
            with binary_path.open(mode="r", encoding="utf-8") as f:
                raw = json.dumps(json.load(f), separators=(',', ':')).encode("utf-8")
        elif binary_path.suffix in [".htm", ".html", ".js", ".css", ".svg"]:
            with open(binary_path, 'r', encoding="utf-8") as f:
                content = f.read()
                if ".min" not in str(binary_path):
                    content = cls._safe_minify(content, binary_path.suffix)
            raw = content.encode("utf-8")
        else:
            # true binary assets (woff2, png, gif, ...) must be read as raw bytes
            with open(binary_path, "rb") as f:
                raw = f.read()

        stinfo = os.stat(binary_path)
        data = gzip.compress(raw, mtime=stinfo.st_mtime)

        with header_path.open(mode="a", encoding="utf-8") as header_file:
            # Derive a unique, valid C identifier from the full relative path.
            # The basename stem alone collides across directories/extensions
            # (e.g. js/bootstrap.bundle.min.js vs css/bootstrap.min.css, or the
            # two themes' style.min.css / 32px.png), so use the whole URI.
            varName = re.sub(r'[^a-zA-Z0-9]', '_', info["uri"].strip('/'))
            if varName and varName[0].isdigit():
                varName = "_" + varName
            header_file.write(f"static const uint8_t {varName}_BIN[] = {{\n    ")
 
            size = 0
            for d in data:
                header_file.write("0x{:02X},".format(d))
                size += 1
                if not (size % 20):
                    header_file.write("\n    ")
            header_file.write("\n};\n\n")

            info["size"] = size
            info["variable"] = f"{varName}_BIN"
            return info

    def process(self):
        print(f"--- GENERATING HTML BINARY HEADERS -> {OUTPUT_DIR} ---")
        binary_header = OUTPUT_DIR / "HTMLbinary.h"

        yaml_src = ROOT_DIR / "REST-API.yaml"
        if yaml_src.exists():
            shutil.copy2(yaml_src, HTML_DIR / "REST_API.yaml")

        if binary_header.exists():
            os.remove(binary_header)

        file_list = []
        for binary_file in BINARY_FILES:
            file_path = HTML_DIR / binary_file
            if not file_path.exists():
                print(f"  Warning: {file_path} not found.")
                continue

            print(f"  Encoding: {binary_file.as_posix()}")
            info = {
                "uri": "/" + binary_file.as_posix(),
                "mimeType": mimetypes.types_map.get(file_path.suffix, "application/octet-stream")
            }
            if file_path.suffix in [".yaml", ".yml"]:
                info["mimeType"] = "application/yaml"
            elif file_path.suffix == ".woff2":
                info["mimeType"] = "font/woff2"
            elif file_path.suffix == ".svg":
                info["mimeType"] = "image/svg+xml"

            processed_info = self._process_binary_file(file_path, binary_header, info)
            file_list.append(processed_info)

        with binary_header.open(mode="a", encoding="utf-8") as f:
            f.write("#pragma once\n")
            f.write("#include <Arduino.h>\n")
            f.write("#include <functional>\n\n")
            f.write("using RouteRegistrationHandler = std::function<void(const String& uri, const String& contentType, const uint8_t * content, size_t len)>;\n\n")
            f.write("class WWWData {\n    public:\n        static void registerRoutes(RouteRegistrationHandler handler) {\n")
            for item in file_list:
                f.write(f'            handler("{item["uri"]}", "{item["mimeType"]}", {item["variable"]}, {item["size"]});\n')
            f.write("        }\n};\n")

HtmlHeaderProcessor().process()