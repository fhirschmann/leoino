# Stability audit — November 2026

Scope: full-system crash, heap-corruption, watchdog, stack-overflow, and brownout hardening across
networking/WebSockets, async JSON/HTTP handlers, PSRAM/ISR cache safety, audio/Bluetooth,
RFID/port-expander/LED lifecycle, SD/WebDAV/FTP/sync workers, dual-core ESP32 QEMU emulation,
and physical hardware stress testing.

## Findings and fixes

| Area | Evidence | Fix / regression check |
| --- | --- | --- |
| WebSocket OOB write & cross-core races | `onWebsocketEvent` (`WS_EVT_DATA`) wrote `data[len] = 0` one byte past `ESPAsyncWebServer`'s exact-sized `new uint8_t[info->len]` DRAM allocation — directly corrupting adjacent TLSF block headers (`insert_free_block`) on WebSocket control/volume frames. `Web_SendWebsocketData` and `ws.cleanupClients` also ran unsynchronized from `loopTask` (Core 1) against `async_tcp` (Core 0), and `WS_DISCONNECT` read 1 byte past short close frames. | Removed `data[len] = 0`; dispatched all WebSocket broadcasts and 1 Hz client cleanup onto `async_tcp` via `espuino_async_tcp_service_hook()`; bounded `WS_DISCONNECT` (`datalen >= 2`, exact length). Exercised under multi-thread WS + malformed 0/1/2-byte close-frame storms in QEMU and on hardware. |
| Async JSON & NVS deserialization | `AsyncCallbackJsonWebHandler` allocated bodies in internal DRAM prior to auth checks and overflowed `_content` when chunked requests forged a small `X-Expected-Entity-Length`. `WiFiSettings::deserialize` dereferenced `nullptr` on 0-byte NVS blobs and read unaligned/un-terminated strings. `ps_malloc` had no DRAM fallback when PSRAM was absent or exhausted. | Replaced all 12 JSON body routes with `SafeJsonWebHandler` (pre-body auth check, `POST/PUT/PATCH` filter, bounded dynamic PSRAM growth, PSRAM AST, immediate `free(_tempObject)`) and all 17 JSON responses with `PsramJsonResponse`. Switched `MemX`/`SpiRamAllocator` to `heap_caps_*_prefer(..., SPIRAM, DEFAULT)`. Added full bounds/NUL validation to `WiFiSettings::deserialize`. Verified under ASan/UBSan in `test_stability.py` and in QEMU. |
| ISR cache-disabled safety & DRAM budget | Live dual-core QEMU ESP32 emulation caught an `EXCCAUSE 7` cache-disabled panic when `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=64` placed ~108-byte `calloc` driver structs in PSRAM (`0x3f800xxx`) and an `IRAM_ATTR` ISR fired on Core 1 while Core 0 disabled flash/PSRAM cache in `nvs::Page::eraseEntryAndSpan`. | Set `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256` so small ISR-touched driver allocations remain in internal DRAM, while moving large upload, WebDAV, FTP, MediaHub, `/playstats`, and FATFS LFN (`CONFIG_FATFS_LFN_HEAP=y`) buffers to PSRAM. Internal free DRAM on hardware doubled from `49.7 KB` to `99.2 KB`. |
| Pre-`setup()` Wi-Fi & brownout protection | QEMU boot tracing showed `HomeSpan` overriding Arduino's weak `init()` to call `WiFi.mode(WIFI_STA)` before `setup()`, triggering ~350 mA Wi-Fi RF calibration before `Led_PrepareForBoot()`, `Port_Init()`, and `SdCard_Init()`, and consuming ~40 KB DRAM even in offline/Bluetooth/LPCD modes. | Patched `Span::init()` in `patchAudioLib.py` to defer Wi-Fi exclusively to firmware control; replaced `WiFi.getMode()` lazy-init probes with `Wlan_IsApMode()`; enabled hardware brownout reset at Level 5 (~2.67 V); fixed PCA9555 shutdown latch-before-direction glitch. |
| Audio, Bluetooth & library parsers | `ESP32-audioI2S` used a tight `4100 B` stack, left `Audio::setTone` unguarded against `mutex_audioTask`, and lacked div-by-zero / underflow / bounds checks in `MP3_XING`, `parsePlaylist_PLS`, `M4A_ESDS`, and `M4A_ILST`. `ESP32-A2DP` truncated `esp_avrc_tg_cb_param_t` via `sizeof(esp_a2d_cb_param_t)`. `Bluetooth.cpp` allocated `std::vector` heap memory inside a spinlock and mutated I2S/volume from BTC callbacks. | Patched `ESP32-audioI2S` (`6144 B` stack, `setTone` mutex, parser guards) and `ESP32-A2DP` in `patchAudioLib.py`. Deferred cross-task `setVolume`/`setTone` and BT callbacks to `loopTask`, guarded track metadata with `gMetadataMutex`, replaced BT scan spinlock with a FreeRTOS mutex + 32-device cap, and removed fatal `while(1)` on I2S init failure. |
| Task lifecycle, sync WDT & RFID/PN5180 | `Led` and `Rfid` used `vTaskSuspend`/`vTaskDelete` externally while tasks could hold I2C/SPI/NVS locks. `Sync.cpp` held its `/Playlists` mutex across 20 s HTTP downloads, starving `async_tcp` into a TWDT panic. Auto-detecting no RFID reader defaulted to MFRC522 polling forever. `ipc0` stack watermark dropped to `264 B`. | Cooperative atomic pause/exit handshakes in `Led` and `Rfid`; `RfidReaderType::TYPE_NONE` skips task spawn when no reader is wired; narrowed `Sync.cpp` mutex to local SD file ops; bounded recursion and closed leaked `File` handles in `SdCard`/`MediaHub`/`WebExplorer`/`Webdav`; hardened `PN5180` register/ISO15693 bounds and FTP session/buffer safety; raised `ipc`, `sys_evt`, `tiT`, `btu`/`btc`, `loopTask`, and worker stacks. |
| Log ring | Six concurrent writers plus snapshots reproduce index 2048 outside the 2048-byte buffer in the pinned library under UBSan. | Shared static mutex and single-lock line write in `Log_Printf`; 60,000 messages and concurrent snapshots in `test_log_concurrency.py`. |
| Volume LUT | Regular 0…1 inputs are bounded; infinity reaches an undefined float-to-int conversion in the previous function. | Clamp before conversion; NaN maps to minimum. Both curves, all 256 setting values, 2.56M ramp points and 256k maximum/back cycles tested. |
| ID3 title & cover parser | Truncated `Title/` strings caused an ASan heap-buffer-overflow; cover endpoints lacked EOF/FLAC-wraparound/short-read guards. | Delimiter search, paired range validation, bounded reads/writes, EOF and 24-bit FLAC length wraparound checks; verified in `test_stability.py` and QEMU `/cover` fuzzing. |
| LED SPI/RMT | With a timeout injected, the old driver overwrites/requeues a pending transfer. | Preserve DMA buffer/transaction ownership until success, retain dirty frames, retry failed enqueue; guarded `numIdleDots == 0` / `dimmableStates == 0`. |
| Upload buffers & errors | Shared global buffers could be reset or freed on disconnect; unchecked worker startup, partial-write duplication, and `free()` allocator mismatch on error objects. | PSRAM upload buffers, single request owner, `x_malloc` + static `uploadFailedRequest` fallback matching `~AsyncWebServerRequest`, 250 ms WDT-safe wait slices, disconnect-safe response. Verified in `test_upload_stability.py`. |
| Task diagnostics | Fixed 2048-byte unchecked formatter buffer on command path; `/stats` relied on estimated text capacity. | Bounded `uxTaskGetSystemState` snapshot, checked allocation, direct output and zero-runtime guard. |

## Reproduction

```sh
python3 tools/test_stability.py
python3 tools/test_upload_stability.py
python3 tools/test_log_concurrency.py  # needs complete dependencies already fetched
python3 tools/test_espuino_helper.py
python3 tools/check_demo_endpoints.py
UV_CONSTRAINT="$PWD/tools/build-constraints.txt" pio run -e complete -j 4
python3 tools/test_qemu_esp32.py       # builds [env:qemu] and runs dual-core ESP32 QEMU stress suite
```

`tools/test_stability.py`, `tools/test_upload_stability.py`, and `tools/test_log_concurrency.py`
compile production C++ paths on the host under AddressSanitizer and UndefinedBehaviorSanitizer.
`tools/test_qemu_esp32.py` boots the full dual-core Xtensa ESP32 firmware (`[env:qemu]`, rev 3.1,
4 MB PSRAM, 16 MB flash, 64 MB FAT32 `SD_MMC`, OpenCores Ethernet) inside Espressif QEMU and
runs live HTTP, chunked-JSON overflow, cover-parser, WebDAV, and multi-thread WebSocket /
malformed close-frame fuzzing while asserting zero panics, zero reboots, and intact heap watermarks.

## Device evidence, hardware stress test, and remaining limits

1. **Historical `AT-1` crash dump (`insert_free_block` in `loopTask`):**
   The previously retrieved dump reported TLSF free-list corruption (`insert_free_block`), whose
   signature directly matches an adjacent block header being overwritten by a 1-byte out-of-bounds
   write in internal DRAM — the exact bug found and fixed in `onWebsocketEvent` (`data[len] = 0` on
   every incoming WebSocket text frame, including volume changes), compounded by cross-core
   `Web_SendWebsocketData` / `ws.cleanupClients` calls from `loopTask`. Because that historical
   dump's ELF SHA prefix (`5551af21f`) predated symbol archiving, the dump itself was preserved
   untouched rather than decoded against mismatching symbols.

2. **Development board (`AT-2`, `ESP32-D0WD-V3`, 16 MB Flash, 4 MB PSRAM, 64 GB SD_MMC) telemetry:**
   Flashing `complete` over USB serial and running simultaneous SD WAV loop playback, 3-thread
   rapid WebSocket volume sweeps, malformed `WS_DISCONNECT` frames, chunked `X-Expected-Entity-Length`
   overflow payloads, and `/cover` ID3v2/FLAC fuzzing produced **0 crashes, 0 resets, and 0 core dumps**:

   | Metric (`AT-2` `/debug`) | Before hardening | After hardening (post-stress) |
   | --- | --- | --- |
   | Internal `freeHeap` | `49,731 B` | `99,207 B` idle / `96,659 B` post-stress (`minFreeHeap`: `84,132 B`, `largestFreeInternalBlock`: `65,536 B`) |
   | `ipc0` stack free watermark | `264 B` | `856 B` |
   | `tiT` (lwIP) stack free watermark | `992 B` | `2,192 B` |
   | `sys_evt` stack free watermark | `1,556 B` | `2,416 B` |
   | `loopTask` / `PeriodicTask` / `async_tcp` free watermarks | `2,140 B` /tight /shared | `9,276 B` / `3,959 B` / `3,256 B` |

3. **Remaining physical limits:**
   High-current speaker transients on battery power, physical rotary-encoder contact bounce, and
   multi-day thermal/EMI behavior still depend on the target enclosure and power rail; hardware
   brownout detection (`~2.67 V`) now forces a clean reset before undervoltage flash/RAM corruption.
