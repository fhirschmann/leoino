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
> Bluetooth, backups and convenience. For hardware, wiring and general documentation please refer
> to the [original documentation](https://forum.espuino.de/c/dokumentation/anleitungen/10).

## // Differences to upstream

All changes compared to upstream/`dev`, each with a reference to its commit:

### Web interface

| Change | Commit |
| --- | --- |
| Complete cyberpunk redesign (neon palette, scanlines, Orbitron/Rajdhani/Share Tech Mono, custom login/AP pages) | [`8c06be5`](../../commit/8c06be5) |
| Cyberpunk styling for the upstream Bluetooth scan UI | [`959cdb6`](../../commit/959cdb6) |
| Navbar branding: "Leo Industries AT-1" + hostname in two colors, company name hidden on small screens | [`7024590`](../../commit/7024590) |
| Custom neon logo (angular emblem with L monogram) used as logo and SVG favicon | [`4d410a6`](../../commit/4d410a6) |
| PWA support: web app manifest + app icon, "add to home screen" with proper icon and name | [`65df91b`](../../commit/65df91b) |
| Password protection: single password (no username), 90-day session cookie, logout menu entry, brute-force lockout; inactive in hotspot mode | [`cf77b0f`](../../commit/cf77b0f) |
| Drag & drop: upload files by dropping them from the file manager onto the file tree | [`079bf30`](../../commit/079bf30) |
| SD card cleanup: removes macOS metadata (`.DS_Store`, `._*`, Spotlight/Trashes) with one click | [`3558d5c`](../../commit/3558d5c) |
| SD card capacity gauge in the files tab and the info dialog | [`19fd4e8`](../../commit/19fd4e8) |
| Battery indicator in the navbar with a low-battery warning toast | [`85b2a97`](../../commit/85b2a97) |
| Live log: the log dialog refreshes every 2 s and follows the end of the log | [`bfc30ef`](../../commit/bfc30ef) |
| Log download as a text file | [`f2d98dc`](../../commit/f2d98dc) |
| Full backup: export/import of all settings + RFID assignments as JSON, WiFi credentials optional | [`3affe72`](../../commit/3affe72) |
| FTP server can be stopped from the web interface (start button turns into a stop button) | [`293bb20`](../../commit/293bb20) |
| Bluetooth modes can be stopped from the web interface; commands 143/144 switch modes directly via buttons | [`aec80ee`](../../commit/aec80ee) |

### RFID & audio

| Change | Commit |
| --- | --- |
| Tag removal detected via consecutive-miss counter instead of a wall-clock timeout: pause after ~0.5 s, immune to phantom dropouts and task starvation | [`a223f30`](../../commit/a223f30) |
| Vendored PN5180 library with fast no-card detection: read attempts on an empty field take ~25 ms instead of ~230 ms (no more 200 ms timeout) | [`8f92689`](../../commit/8f92689) |
| SLIX2 password support, configurable idle LED and progress bar colors | [`1d59edb`](../../commit/1d59edb) |
| Ready sound on cold start | [`e2738ca`](../../commit/e2738ca) |
| Cyberpunk "Data Drop" idle LED animation | [`a4722b4`](../../commit/a4722b4) |
| Improved button responsiveness and track navigation seek options | [`32065ab`](../../commit/32065ab) |
| Unlocking controls via button press while locked | [`52d22d7`](../../commit/52d22d7) |
| Support for a 6th button | [`b116151`](../../commit/b116151) |

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
