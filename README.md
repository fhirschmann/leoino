<div align="center">

<img src="html/logo.svg" width="130" alt="Leo Industries AT-1 logo">

# LEO INDUSTRIES AT-1

`// RFID AUDIO PLAYER :: ESP32 :: CYBERPUNK EDITION`

[![Base](https://img.shields.io/badge/Base-ESPuino%20(dev)-00f0ff?style=flat-square&labelColor=05070d)](https://github.com/biologist79/ESPuino)
[![License](https://img.shields.io/badge/License-GPL--3.0-ff2a6d?style=flat-square&labelColor=05070d)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32%20%2B%20PN5180-00f0ff?style=flat-square&labelColor=05070d)](platformio.ini)

</div>

> **LEO INDUSTRIES AT-1** is a private fork of [ESPuino](https://github.com/biologist79/ESPuino)
> (branch `dev`) — an RFID-controlled audio player based on the ESP32. This fork gives the web
> interface a complete cyberpunk overhaul and adds a number of features around RFID detection,
> Bluetooth, backups and convenience. For the upstream hardware, wiring and general documentation
> please refer to the [original documentation](https://forum.espuino.de/c/dokumentation/anleitungen/10).
>
> ⚡ **Full disclosure:** the firmware and web interface in this fork are largely *vibe-coded*
> (AI-assisted). The hardware is not — the enclosure was designed by hand in CAD without any AI.
> The printable STL files live in [`stl/`](stl/).

---

# // HARDWARE

The physical build — a 3D-printed enclosure housing an ESP32, a PN5180 RFID reader, speaker and
battery. The case was modelled by hand in CAD (no AI involved); all printable parts — case,
panels, lids, handle, rotary knob, keycaps and the RFID cartridge — are available as STL files in
[`stl/`](stl/).

## Bill of materials

| Part | Details | Source |
| --- | --- | --- |
| Mainboard | ESPuino **complete** board (rev 5.1) | [forum.espuino.de](https://forum.espuino.de/t/espuino-complete/3817) |
| Headphone amplifier board | biologist79 **MS6324 + TDA1308 / LM4808M** board | [forum.espuino.de](https://forum.espuino.de/t/kopfhoererplatine-basierend-auf-ms6324-und-tda1308-bzw-lm4808m/1099) |
| Rotary-encoder board | **Drehencoder by ESPuino** | [forum.espuino.de](https://forum.espuino.de/t/drehencoder-by-espuino/2414) |
| RFID reader | NXP **PN5180** (JST 2.5 mm socket soldered on) | [AliExpress](https://de.aliexpress.com/item/1005006781712003.html) |
| RFID tags | one tag per cartridge | [Amazon](https://www.amazon.de/dp/B0CSJST6KZ) |
| Display | **OLED** 128×64, I2C (SH1106 / SSD1306) | [AliExpress](https://www.aliexpress.com/item/1005006862867338.html) |
| Speaker | **Peerless by Tymphany TC7FD00-04** | [SoundImports](https://www.soundimports.eu/de/peerless-by-tymphany-tc7fd00-04.html) |
| Battery | **LiFePO₄ 3.2 V 6000 mAh** pack with protection, JST-PH 2.0 | [eremit.de](https://www.eremit.de/p/3-2v-6000mah-pack-mit-schutz-arduino-aio-jst-ph-2-0-stecker) |
| Status LEDs | 2× **8-LED WS2812B** (NeoPixel) | [Amazon](https://www.amazon.de/dp/B09YTLY6CK) |
| Standby LED | **white breathing LED** | [AliExpress](https://de.aliexpress.com/item/1005005336879647.html) |
| Internal USB tap | adapter to tap USB off the complete board | [AliExpress](https://de.aliexpress.com/item/1005009847773743.html) |
| External USB port | panel-mount socket that exposes an external USB port and passes the 4 USB lines through to the internal connector | [AliExpress](https://de.aliexpress.com/item/1005009015653966.html) |
| Power switch | latching switch | [AliExpress](https://de.aliexpress.com/item/4001099324784.html) |
| Key switches | **Kailh BOX Navy** (clicky) for the panel buttons | [whackydesks](https://whackydesks.com/produkt/kailh-box-navy/) |
| Magnets | 4× **10×3 mm** per cartridge | — |
| Screws | 50× button-head **ISO 7380 A2 M3×8**<br>25× thermoplastic self-tapping **2.5×10 TORX, black A2** | [screwsandmore](https://www.screwsandmore.de) |
| Sealing | Kafuter **K-704B** + **K-705**, transparent | — |

### Filament & finishing

- **Extrudr XPETG Matte** — Metallic and Black
- **Extrudr PETG** — Turquoise and Copper

Many of the JST/connector cables were hand-crimped with **ENGINEER PA-09** crimping pliers.

> 🚧 _Still to add: photos of the printed unit (front / open / internals) and wiring/pinout notes._

---

# // SOFTWARE

The firmware is based on ESPuino's `dev` branch with a cyberpunk web interface and a set of
fork-specific features. The full interface (default RFID tab shown below, live from the device):

<div align="center">

<img src="docs/img/rfid-tab.png" width="720" alt="Cyberpunk web interface — RFID tab">

</div>

## // Differences to upstream

All changes compared to upstream/`dev`, each with a reference to its commit.

### Web interface

The management and access point pages were completely rebuilt in a cyberpunk style — neon
palette, scanlines, `Orbitron`/`Rajdhani`/`Share Tech Mono` typography, a custom login page, the
upstream Bluetooth scan UI restyled to match, the device branding in the navbar and an embedded
neon logo that doubles as the SVG favicon ([`7be5254`](../../commit/7be5254)):

<div align="center"><img src="docs/img/feat-navbar.png" width="760" alt="Navbar branding"></div>

| Change | Commit |
| --- | --- |
| PWA support: web app manifest + app icon, "add to home screen" with proper icon and name | [`b4287b9`](../../commit/b4287b9) |
| PWA offline fallback: a service worker serves a cyberpunk "ESPuino Offline" page (with auto-reconnect) instead of a black screen when the home-screen app is launched while the player is powered off | [`bd07a7c`](../../commit/bd07a7c) |
| Full backup: export/import of all settings + RFID assignments as JSON, WiFi credentials optional | [`4c90ff4`](../../commit/4c90ff4) |
| One-click OTA update: GitHub Actions publishes a rolling `latest` release (`firmware.bin`); a Tools-tab button makes the device pull that firmware from GitHub over HTTPS and flash it via OTA, then reboot. Also triggerable via the bindable command **186** (button/RFID modifier) and the MQTT command-topic `firmware_update` (`ON`/`update`); the state-topic reports `idle`/`updating`/`up_to_date`/`failed` | [`8527f5e`](../../commit/8527f5e) |
| Equalizer profiles: dropdown presets (Flat / Music / Audiobook-Speech / Deep voices / Custom) on top of the 3-band tone control; speech presets cut bass and lift mids/highs so deep narrator voices stay intelligible, persisted in NVS. Profiles can also be assigned per file or directory (right-click in the file browser) — e.g. set the speech profile for all Bibi Blocksberg episodes at once; the RFID tab shows the active profile for the highlighted file (or "No EQ set"). The active profile can be cycled with the bindable command **154** (button/RFID modifier) and set/reported via the MQTT topic `equalizer` (`flat`/`music`/`speech`/`voiceBoost`) | [`11ade33`](../../commit/11ade33) |
| Blinking "OK" indicator next to the battery replaces the generic "action successful" toast | [`f41bb72`](../../commit/f41bb72) |
| Blinking "connection lost" icon in the navbar replaces the connection-lost toast | [`be13308`](../../commit/be13308) |
| WiFi signal-strength indicator in the navbar (RSSI %, color-coded), next to the battery | [`26e8cf8`](../../commit/26e8cf8) |
| Play/pause button in the RFID tab plays the highlighted file/folder (and pauses running playback) | [`3870bb7`](../../commit/3870bb7) |
| File browser tab renamed to "Files" (folder icon) and the file tree view height doubled for easier navigation | [`3af1fc2`](../../commit/3af1fc2) |
| Cyberpunk footer below the interface (neon "LEO INDUSTRIES // DIVISION: AUDIO" branding) | [`b49f131`](../../commit/b49f131) |
| SD card cleanup: removes macOS metadata (`.DS_Store`, `._*`, Spotlight/Trashes) with one click | [`4e68541`](../../commit/4e68541) |
| Live log: the log dialog refreshes every 2 s and follows the end of the log | [`5183e78`](../../commit/5183e78) |
| Drag & drop: upload files by dropping them from the file manager onto the file tree | [`f8b477b`](../../commit/f8b477b) |
| Password protection: single password (no username), 90-day session cookie, logout menu entry, brute-force lockout; inactive in hotspot mode | [`e74e712`](../../commit/e74e712) |
| Log download as a text file | [`5d9d591`](../../commit/5d9d591) |
| Battery indicator in the navbar with a low-battery warning toast | [`4566ae0`](../../commit/4566ae0) |
| SD card capacity gauge in the files tab and the info dialog | [`45b340a`](../../commit/45b340a) |
| FTP server can be stopped from the web interface (start button turns into a stop button) | [`6ea4020`](../../commit/6ea4020) |
| FTP password shown in settings with a show/hide (eye) reveal toggle (already persisted in NVS) | [`df0a583`](../../commit/df0a583) |
| Bluetooth modes can be stopped from the web interface; commands 143/144 switch modes directly via buttons | [`f16030f`](../../commit/f16030f) |
| Button lock (Kindersicherung) toggle directly in the control tab of the web interface with visual lock status indicator | [`520815a`](../../commit/520815a) |
| Control buttons (single-track Repeat toggle, Sleep-Timer with dropdown and live remaining countdown, Night Mode/Dimming) directly in the control tab of the web interface | [`88a742c`](../../commit/88a742c) |
| Bluetooth-mode dropdown in the control tab (Normal / Speaker / Headphones), mirroring the sleep-timer dropdown; selecting a mode restarts the device into it and the button highlights the active BT mode | [`d57f24a`](../../commit/d57f24a) |
| Mobile-optimized control tab: the hard-to-drag volume slider is replaced by full-width louder/quieter (+ EQ) buttons on phones, and the control header stacks so the legend stays on one line with the action buttons below | [`aef1765`](../../commit/aef1765) |
| System-information dialog rendered as a clean property/value table (instead of preformatted text) and extended with the PN5180 RFID-reader firmware version (read once from the reader's EEPROM at init and exposed via `/info`) | [`715d867`](../../commit/715d867) |
| Rolling build version (`rN` from the commit count, embedded via `gitVersion.py` and published in the release `version.json`) shown as a navbar badge — green when the device runs the latest rolling release, amber when an update is available (passive `/version` check); clicking the amber badge starts the GitHub OTA; also listed in the info dialog | [`b736abc`](../../commit/b736abc) |

#### Feature highlights

| | |
| --- | --- |
| <img src="docs/img/feat-sdcapacity.png" width="320" alt="SD capacity gauge"> | **SD capacity gauge** — free / total space below the file browser ([`45b340a`](../../commit/45b340a)) |
| <img src="docs/img/feat-battery.png" width="90" alt="Battery indicator"> | **Battery indicator** — live charge level in the navbar ([`4566ae0`](../../commit/4566ae0)) |
| <img src="docs/img/feat-cleansd.png" width="90" alt="SD clean button"> | **SD cleanup** — one click removes macOS metadata junk ([`4e68541`](../../commit/4e68541)) |
| <img src="docs/img/feat-slix2.png" width="320" alt="SLIX2 password field"> | **SLIX2 password** — read protected ICODE-SLIX2 tags ([`d3cc69c`](../../commit/d3cc69c)) |
| <img src="docs/img/feat-equalizer.png" width="300" alt="Equalizer with presets"> | **Equalizer presets** — Flat / Music / Audiobook-Speech / Deep voices / Custom on top of a 3-band tone control ([`11ade33`](../../commit/11ade33)) |
| <img src="docs/img/feat-controls.png" width="430" alt="Control-tab buttons"> | **Control buttons** — single-track Repeat, Sleep-Timer, Night Mode, Bluetooth and lock, directly in the control tab ([`88a742c`](../../commit/88a742c)) |
| <img src="docs/img/feat-control-bt.png" width="430" alt="Bluetooth-mode dropdown"> | **Bluetooth-mode dropdown** — switch Normal / Speaker / Headphones from the control tab (the active mode is hidden) ([`d57f24a`](../../commit/d57f24a)) |

### RFID & audio

| Change | Commit |
| --- | --- |
| Tag removal detected via consecutive-miss counter instead of a wall-clock timeout: pause after ~0.5 s, immune to phantom dropouts and task starvation | [`1fad9cd`](../../commit/1fad9cd) |
| Vendored PN5180 library with fast no-card detection: read attempts on an empty field take ~25 ms instead of ~230 ms (no more 200 ms timeout) | [`8762784`](../../commit/8762784) |
| SLIX2 password support for protected ICODE-SLIX2 tags | [`d3cc69c`](../../commit/d3cc69c) |
| Configurable idle LED and progress bar colors | [`bdc54e5`](../../commit/bdc54e5) |
| Ready sound on cold start | [`c051c40`](../../commit/c051c40) |
| Cyberpunk "Data Drop" idle LED animation | [`f20b111`](../../commit/f20b111) |
| Selectable idle animation (standard idle dots or cyberpunk "Data Drop") in the LED settings; defaults to standard | [`f6c3f4e`](../../commit/f6c3f4e) |
| Improved button responsiveness and track navigation seek options | [`76e1535`](../../commit/76e1535) |
| Unlocking controls via button press while locked | [`d83e15f`](../../commit/d83e15f) |
| Support for a 6th button | [`b116151`](../../commit/b116151) |
| OLED display support (SH1106/SSD1306 128×64 over I2C): boot splash, idle screen with IP + READY, now-playing title (up to 3 lines, scrolling) with battery/time/wifi status bar, and a volume bar | [`8ce8104`](../../commit/8ce8104) |

## // Flashing

```bash
pio run -e complete -t upload
```

The web interface (HTML, locales, manifest, icons) is embedded into the firmware automatically
during the build. Alternatively use OTA: Tools → firmware update with
`.pio/build/complete/firmware.bin`.

## // Upstream sync

The fork follows upstream/`dev`. The remote is already set up:

```bash
git fetch upstream
git rebase upstream/dev
```

## // License

Same as the original: [GPL-3.0](LICENSE). The original README content (hardware, HALs, wiring)
can be found in the [ESPuino documentation](https://github.com/biologist79/ESPuino#readme).
