#!/usr/bin/env python3
"""Guard against GitHub-Pages demo drift.

The browser demo (https://fhirschmann.github.io/leoino/) ships a mock REST/WebSocket layer in
tools/demo/demo-mock.js. Whenever a new HTTP endpoint is added to the firmware but not to the mock,
the demo silently breaks for that feature. This check compares the endpoints registered in
src/Web*.cpp against what the mock answers, and fails if a firmware endpoint is neither mocked nor
explicitly allow-listed as "intentionally not part of the demo".

Run locally:  python3 tools/check_demo_endpoints.py
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "tools", "demo", "demo-mock.js")

# Endpoints that legitimately have no place in the device-free demo: static assets, file
# downloads/streams, binary/OTA, hardware-only actions. Adding a new endpoint here is a conscious
# "the demo doesn't need this" decision — keep the reason next to it.
ALLOWLIST = {
    "/": "static management page (served as a real file in the demo)",
    "/favicon.ico": "static asset",
    "/logo": "static asset",
    "/cover": "cover image is served from WebSocket trackinfo in the demo",
    "/explorerdownload": "file download — no real SD card in the demo",
    "/githubupdate": "OTA firmware update — no device to flash in the demo",
    "/update": "OTA firmware binary upload — no device to flash in the demo",
    "/log": "live serial-log stream — nothing to stream in the demo",
    "/debug": "task/heap debug dump — device-only",
    "/inithalleffectsensor": "hardware calibration — device-only",
    "/sdclean": "SD-card cleanup — no real SD card in the demo",
    "/stats.csv": "CSV download of listening stats — not exercised by the demo",
    "/wifiscan": "live WiFi scan — device-only",
    "/wificonfig": "WiFi credential write — device-only",
    "/playlist": "writes an .m3u to the SD card — no real SD card in the demo",
    "/eqrule": "per-path EQ rule write — not exercised by the demo",
    "/rfidresetpos": "audiobook resume reset — not exercised by the demo",
    "/rfidnvserase": "wipe all RFID assignments — destructive, device-only",
    "/inithalleffectsensor": "hardware calibration — device-only",
    "/homekit/qr.svg": "HomeKit pairing QR — device-only",
    "/homekit/reset": "HomeKit unpair — device-only",
    "/homekit/regencode": "HomeKit code regen — device-only",
    "/homekit/enable": "HomeKit enable/disable — device-only",
    "/security": "login/password change — the demo has no auth",
    "/login": "login — the demo has no auth",
    "/stats": "websocket-pushed stats; REST variant not used by the demo UI",
}


def firmware_endpoints():
    paths = set()
    pat = re.compile(r'(?:\.on|AsyncCallbackJsonWebHandler)\(\s*"(/[^"]*)"')
    for f in glob.glob(os.path.join(ROOT, "src", "Web*.cpp")):
        with open(f, encoding="utf-8") as fh:
            for m in pat.finditer(fh.read()):
                paths.add(m.group(1))
    return paths


def mock_coverage():
    """Return (exact_paths, regex_tokens) the mock answers."""
    with open(MOCK, encoding="utf-8") as fh:
        src = fh.read()
    # exact `p === "/x"` (and `p.startsWith("/x")`) comparisons
    exact = set(re.findall(r'p\s*===\s*"(/[^"]*)"', src))
    exact |= set(re.findall(r'p\.startsWith\(\s*"(/[^"]*)"', src))
    # the write no-op regex: /^\/(a|b|c)\b/  -> tokens a,b,c
    tokens = set()
    for grp in re.findall(r'/\^\\/\(([^)]*)\)', src):
        for tok in grp.split("|"):
            tok = tok.strip()
            if tok:
                tokens.add("/" + tok)
    return exact, tokens


def is_covered(endpoint, exact, tokens):
    if endpoint in exact:
        return True
    # mirror the JS regex semantics: /^\/token\b/
    for tok in tokens:
        if re.match(re.escape(tok) + r"\b", endpoint):
            return True
    return False


def main():
    fw = firmware_endpoints()
    if not fw:
        print("ERROR: no firmware endpoints found — did the source layout change?", file=sys.stderr)
        return 2
    exact, tokens = mock_coverage()

    missing = sorted(e for e in fw if not is_covered(e, exact, tokens) and e not in ALLOWLIST)
    stale = sorted(a for a in ALLOWLIST if a not in fw)

    for a in stale:
        print(f"note: allow-listed endpoint no longer exists in firmware: {a}")

    if missing:
        print("\nDemo-mock drift detected — these firmware endpoints are neither mocked in")
        print("tools/demo/demo-mock.js nor allow-listed in tools/check_demo_endpoints.py:\n")
        for e in missing:
            print(f"  {e}")
        print("\nFix: add a handler to demo-mock.js (so the demo works), or add the endpoint to")
        print("ALLOWLIST in tools/check_demo_endpoints.py with a reason if it has no place in the demo.")
        return 1

    print(f"OK: all {len(fw)} firmware endpoints are mocked or allow-listed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
