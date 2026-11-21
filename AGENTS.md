# Developer Guide

> This file is the single source of truth; `CLAUDE.md` is a symlink to it.

You are a developer on **LEO INDUSTRIES AT-1**, a private fork of
[ESPuino](https://github.com/biologist79/ESPuino) (`dev` branch): an RFID-controlled
ESP32 audio player with a fully reworked, cyberpunk web interface. Firmware in C++
(PlatformIO/Arduino), web interface as static HTML/JS, plus a GitHub Pages demo.

> 🔐 **Credentials are NOT in this file.** Read `AGENTS_LOCAL.md` first (not checked in)
> for local device IPs, the web UI password, and personal settings. The helper script
> loads credentials from the env vars `ESPUINO_IP` / `ESPUINO_API_KEY` (legacy
> `ESPUINO_PASS`), `.espuino_config.json`, or the sibling Homelab repo's ignored
> `secrets.json` (`services.leoino.url` / `services.leoino.api_key`). Override that path
> with `HOMELAB_SECRETS_PATH`. Never print or commit secrets.

## Build & Deploy
- Always use the **`complete` environment** to build and flash the ESP32
  (`default_envs = complete` in `platformio.ini`).
- Local builds are flashed over **USB serial** — the web OTA only pulls GitHub releases,
  not your local state.

## Code style
- The **clang-format check** in the GitHub pipeline must pass. Format changed C/C++ code
  against the `.clang-format` in the repo root before committing.

## Features
- For **every feature, update `README.md`** and mention it there.
- Update the **web interface** for the feature wherever it makes sense.
- Where sensible, also build in **MQTT support** and **`COMMANDS`** (for keybindings), and
  keep the **Swagger/REST API** (`html/REST_API.yaml`) up to date.
- Try to make **one commit per feature** that contains both the feature **and** the docs.

## Web interface & GitHub Pages site
`tools/build_demo.py` builds the whole Pages site: the landing page
(`tools/demo/landing.html`) at <https://fhirschmann.github.io/leoino/> and the device-free
Web-UI demo under <https://fhirschmann.github.io/leoino/demo/>.

When you change the web interface (new endpoints, WebSocket fields, settings, UI tabs),
**always update the demo too** — otherwise the demo breaks:
- update the mock in `tools/demo/demo-mock.js` (fake WebSocket + REST)
- update `tools/build_demo.py` if needed
- test locally with `python3 tools/build_demo.py demo_dist`

Landing-page assets (screenshots, photos) come from `docs/img/` — keep `LANDING_IMAGES`
in `tools/build_demo.py` in sync when you rename or add one.

## Git & commits
- You may commit and push directly to **`master`**.
- **No force-pushes.** Already-pushed history is not rewritten; fixes to a feature land as
  **additional commits**. Clean up at most via a squash-merge of a PR.
- Add yourself as **co-author** (unless you are Gemini or Antigravity).
- **No commits with a timestamp on Tue/Thu/Fri 8 AM–6 PM** (working hours) — put them on
  the evening before, for example. **No commits in the future.**
- **Experiment mode** (default: off): no commits, no pushes.

## Content & naming
- Follow the content, branding, and screenshot conventions documented in `AGENTS_LOCAL.md`.

## Helper script `tools/espuino_helper.py`
Automatically filters forbidden words out of its output and translates them back for API requests.
- List files: `python3 tools/espuino_helper.py --list`
- Create a playlist:
  `python3 tools/espuino_helper.py --playlist "/Playlists/NAME" --tracks "/Path1" "/Path2"`

- Any HTTP API route: `python3 tools/espuino_helper.py --api /info`
- Private diagnostic bundle: `python3 tools/espuino_helper.py --diagnostics /private/at1-debug`
- JSON writes: `--api /settings --method POST --data-file /private/request.json`
- Core dumps survive OTA: compare `coredump.appElfSha256` / `firmwareMatches` with the
  running build before decoding. Save the matching release `firmware.elf` with the dump.
