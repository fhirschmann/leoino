# `esp-raop-receiver` — vendored AirPlay 1 (RAOP) receiver

This ESP-IDF component implements an **AirPlay 1 / RAOP audio receiver** (the
device shows up as an AirPlay speaker for iPhone/iPad/Mac). It is consumed by
`src/AirPlay.cpp` in the firmware, which feeds the decoded PCM into the same I2S
DAC path used by the Bluetooth A2DP sink mode.

## Why AirPlay 1 (and not AirPlay 2)

AirPlay 2 receivers (e.g. `rbouteiller/airplay-esp32`) are monolithic firmwares
that require HomeKit (HAP) pairing — which collides with this project's existing
HomeSpan HomeKit accessory — plus PTP timing and a non-commercial licence. They
are not embeddable as a component. AirPlay 1 (RAOP) needs no pairing, decodes a
single ALAC stream, runs comfortably on this WROVER+PSRAM board, and is still
fully supported as an audio target by current iOS/macOS.

## Source provenance (all auditable source — nothing prebuilt)

| Part | Origin | Licence |
| ---- | ------ | ------- |
| RAOP/RTSP/RTP core, jitter buffer, DMAP parser (`src/`) | `Edu_Coder/esp-airsync` (codeberg), itself derived from philippe44's `squeezelite-esp32` RAOP sink | GPL-3.0 / MIT (per-file headers) |
| `esp_raop_receiver.h` callback API + `raop_core.c` wrapper | `Edu_Coder/esp-airsync` | GPL-3.0 |
| ALAC decoder (`alac/ALAC*.{c,cpp,h}`, `ag_dec.c`, `dp_dec.c`, `matrix_dec.c`, `EndianPortable.c`) | Apple's open-source ALAC reference (`macosforge/alac`) | Apache-2.0 |
| `alac/alac_wrapper.cpp` | Written for this project (bridges Apple's `ALACDecoder` to the philippe44 `alac_wrapper.h` API) | MIT |

The upstream `esp-airsync` ships ALAC only as a prebuilt `libalac.a`. That opaque
binary is **deliberately not used here**: it was replaced with Apple's ALAC
reference source compiled from scratch, so every byte linked into the firmware is
reviewable.

## Local modifications

- `src/raop.c`: `_raop._tcp` is now advertised in both managed and external mDNS
  mode. ESPuino already owns `mdns_init()`/the hostname (Wlan.cpp / ESPmDNS), so
  the component runs in `RAOP_MDNS_EXTERNAL` mode but must still register the
  service itself.
- `src/rtp.c` (`rtp_init` / `rtp_t`): the per-session context was ~26 KB and
  `heap_caps_calloc`'d from **internal** RAM on every SETUP. Once the web server / HomeKit /
  websockets fragment the internal heap, that contiguous 26 KB allocation fails and
  `rtp_init` silently returns, so SETUP answers 503 ("missing ports": all RTP ports 0) and
  the sender shows "could not connect" / plays silently. The struct was almost entirely an
  over-provisioned 1252-entry jitter-buffer metadata array (`BUFFER_FRAMES_MAX`); the real
  depth is set at runtime from the SETUP PCM buffer (512 frames), so `BUFFER_FRAMES_MAX` was
  reduced to **512** and the 4 KB in-struct static task stack removed (the RTP task is now
  created **dynamically** with `xTaskCreatePinnedToCore`, its stack allocated from internal
  RAM). That shrinks `rtp_t` to ~9 KB, which still allocates from a fragmented internal heap.
  It is kept in **internal** RAM on purpose: it is in the hot AES-decrypt/ALAC-decode/timing
  path and the ESP32 hardware-AES cannot DMA from PSRAM (an earlier PSRAM attempt caused
  stutter + a crash). Verified with a minimal RTSP client: SETUP returns 200 OK with bound
  `server/control/timing` ports.
- `src/rtp.c` / `src/raop.c`: a few connection-lifecycle / sync / SETUP logs were promoted
  from `LOG_INFO`/`LOG_WARN` to `LOG_ERROR` so they remain visible under
  `CONFIG_LOG_MAXIMUM_LEVEL=ERROR` (which compiles out lower levels) — useful for field
  diagnosis of AirPlay handshakes.
- `CMakeLists.txt`: compiles the Apple ALAC source instead of linking `libalac.a`,
  and defines `TARGET_RT_LITTLE_ENDIAN=1` (Apple's `EndianPortable.c` only
  auto-detects little-endian on x86).
- `src/raop.c` (`rtsp_thread`): the RTSP server originally accepted a single session
  (`listen(sock, 1)`) and never reaped a peer that vanished without a clean TEARDOWN,
  so the slot wedged and every reconnect failed ("could not connect") until a reboot.
  Fixed by (a) also `select()`-ing the listen socket and **preempting** the current
  session when a new sender connects — AirPlay's "take over the speaker" behaviour —
  and (b) enabling TCP keepalive on the accepted socket (~11 s) as a backstop. The
  connection-lifecycle logs were promoted to `LOG_ERROR` so they stay visible at
  `CONFIG_LOG_MAXIMUM_LEVEL=ERROR` (which compiles out `LOG_INFO`).

## Public API

See `include/esp_raop_receiver.h`. The firmware uses `RAOP_VOLUME_SOFTWARE`,
`RAOP_MDNS_EXTERNAL`, an `audio_output_cb` that writes PCM to I2S, and an
`event_cb` for connect/play/stop/metadata.
