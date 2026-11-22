# -*- coding: utf-8 -*-

"""PlatformIO pre-script that validates and hardens pinned third-party libraries.

1. ESP32-audioI2S (v4.0.1):
   - Validates VU/spectrum gates and average-bitrate/seek guarantees.
   - Raises AUDIO_STACK_SIZE from 4100 to 6144 bytes (StackType_t is 1 byte on ESP-IDF).
   - Fixes setAudioFilePosition() unconditional overwrite of m_audioDataStart clamp.
   - Guards MP3 Xing header calculation against samplerate == 0 divide-by-zero.
   - Guards PLS Title parser against missing '=' unsigned length underflow.
   - Guards M4A ESDS DecoderSpecificInfo against zero/truncated length OOB read.
   - Guards M4A ILST metadata parser against zero-length atom infinite loops and OOB reads.
2. ESP32-A2DP (v1.8.11):
   - Fixes AVRCP target event dispatch copying sizeof(esp_a2d_cb_param_t) instead of
     sizeof(esp_avrc_tg_cb_param_t) off the BTC_TASK stack.
3. AsyncTCP (v3.5.0):
   - Exposes a single-task service hook + wake helper on async_tcp so AsyncWebSocket
     broadcasts and cleanupClients() never race AsyncClient lifetimes across cores.
4. ESPAsyncWebServer (v3.12.1):
   - Fixes WS_DISCONNECT close-frame reason parsing OOB read and un-terminated strlen().
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # pylint: disable=undefined-variable

REQUIRED_V4_GUARANTEES = [
    "if (settings.VU_LEVEL) calculateVUlevel(",
    "if (settings.SPECTRUM) calculateSpectrum(",
    "if (m_cab.brCounter > 10)",
    "m_avr_bitrate = calculate_average_bitrate(",
    "m_audioDataReadPtr = (resumeFilePos + offset) - m_audioDataStart;",
]


def libdeps_dir() -> Path:
    return Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")


def find_lib_files(lib_name: str, rel_file: str, required: bool = True) -> list[Path]:
    """Find <lib_name>/<rel_file> and any version-qualified <lib_name>@*/<rel_file>."""
    base = libdeps_dir()

    def _scan() -> list[Path]:
        matches: list[Path] = []
        direct = base / lib_name / rel_file
        if direct.is_file():
            matches.append(direct)
        if base.is_dir():
            for versioned in sorted(base.glob(f"{lib_name}@*")):
                candidate = versioned / rel_file
                if candidate.is_file() and candidate not in matches:
                    matches.append(candidate)
        return matches

    found = _scan()
    if found or not required:
        return found

    print(f"patchAudioLib: {lib_name} not fetched yet, installing lib deps...")
    subprocess.check_call(
        [
            env.subst("$PYTHONEXE"),
            "-m",
            "platformio",
            "pkg",
            "install",
            "-e",
            env.subst("$PIOENV"),
        ],
        cwd=env.subst("$PROJECT_DIR"),
    )
    found = _scan()
    if not found:
        sys.stderr.write(f"patchAudioLib: {lib_name}/{rel_file} not found after install\n")
        sys.exit(1)
    return found


def apply_idempotent_patches(path: Path, patches: list[tuple[str, str, str]]) -> bool:
    source = path.read_text(encoding="utf-8")
    updated = source
    for label, old, new in patches:
        if new in updated:
            continue
        if old not in updated:
            sys.stderr.write(
                f"patchAudioLib: patch '{label}' target not found in {path}.\n"
                "Re-evaluate the library version and update patchAudioLib.py.\n"
            )
            sys.exit(1)
        updated = updated.replace(old, new, 1)
    if updated != source:
        path.write_text(updated, encoding="utf-8")
        return True
    return False


def validate_and_patch_audio_library() -> None:
    paths = find_lib_files("ESP32-audioI2S", "src/Audio.cpp")
    for path in paths:
        source = path.read_text(encoding="utf-8")
        for needle in REQUIRED_V4_GUARANTEES:
            if needle not in source:
                sys.stderr.write(
                    f"patchAudioLib: expected code '{needle}' not found in {path}.\n"
                    "The library changed - re-evaluate the VU/spectrum and seek "
                    "guarantees, then update patchAudioLib.py.\n"
                )
                sys.exit(1)

    patches = [
        (
            "audio-stack-size",
            "constexpr size_t AUDIO_STACK_SIZE = 4100;",
            "constexpr size_t AUDIO_STACK_SIZE = 6144; // patched: StackType_t is 1 byte on ESP-IDF",
        ),
        (
            "seek-clamp-else",
            """    if (pos < m_audioDataStart) {
        AUDIO_LOG_WARN("set audiodatastart at {}", m_audioDataStart);
        m_resumeFilePos = m_audioDataStart;
    }
    m_resumeFilePos = pos;""",
            """    if (pos < m_audioDataStart) {
        AUDIO_LOG_WARN("set audiodatastart at {}", m_audioDataStart);
        m_resumeFilePos = m_audioDataStart;
    } else {
        m_resumeFilePos = pos;
    }""",
        ),
        (
            "settone-mutex",
            """void Audio::setTone(float gainLowPass, float gainBandPass, float gainHighPass) {

    // gainLowPass   set between -12 ... +12 dB
    // gainBandPass  set between -12 ... +12 dB
    // gainHighPass  set between -12 ... +12 dB

    m_audio_items.gain_ls_db = fminf(fmaxf(gainLowPass, -12.0f), 12.0f);
    m_audio_items.gain_peq_db = fminf(fmaxf(gainBandPass, -12.0f), 12.0f);
    m_audio_items.gain_hs_db = fminf(fmaxf(gainHighPass, -12.0f), 12.0f);

    IIR_calculateCoefficients();
}""",
            """void Audio::setTone(float gainLowPass, float gainBandPass, float gainHighPass) {

    // gainLowPass   set between -12 ... +12 dB
    // gainBandPass  set between -12 ... +12 dB
    // gainHighPass  set between -12 ... +12 dB
    if (mutex_audioTask) {
        xSemaphoreTake(mutex_audioTask, portMAX_DELAY);
    }
    m_audio_items.gain_ls_db = fminf(fmaxf(gainLowPass, -12.0f), 12.0f);
    m_audio_items.gain_peq_db = fminf(fmaxf(gainBandPass, -12.0f), 12.0f);
    m_audio_items.gain_hs_db = fminf(fmaxf(gainHighPass, -12.0f), 12.0f);

    IIR_calculateCoefficients();
    if (mutex_audioTask) {
        xSemaphoreGive(mutex_audioTask);
    }
}""",
        ),
        (
            "mp3-xing-samplerate-div0",
            "if (xingPos > 0 && layerIndex == 1) { // layer III only",
            "if (xingPos > 0 && layerIndex == 1 && samplerate > 0) { // layer III only",
        ),
        (
            "pls-title-underflow",
            """            if (m_playlistContent[i].starts_with_icase("Title")) {
                pos = m_playlistContent[i].index_of("=");
                seq_str = m_playlistContent[i].substr(5, pos - 5);""",
            """            if (m_playlistContent[i].starts_with_icase("Title")) {
                pos = m_playlistContent[i].index_of("=");
                if (pos < 5) continue; // patched: guard malformed PLS Title line
                seq_str = m_playlistContent[i].substr(5, pos - 5);""",
        ),
        (
            "m4a-esds-bounds",
            """        int32_t dec_specific_offset = esds_buffer.special_index_of("\\x05\\x80\\x80\\x80", 4, m_m4aHdr.sizeof_esds);
        if (dec_specific_offset >= 0) {                                                           // decoderSpecificInfo found
            uint8_t dec_specific_length = ((uint8_t*)esds_buffer.get())[dec_specific_offset + 4]; // Länge nach Tag + 3 Extended Length Bytes
            AUDIO_LOG_DEBUG("DecoderSpecificInfo found at offset {}, length: {}", dec_specific_offset, dec_specific_length);

            // extract decoderSpecificInfo-data
            ps_ptr<char> dec_specific_data;
            dec_specific_data.alloc(dec_specific_length, "dec_specific_data");""",
            """        int32_t dec_specific_offset = esds_buffer.special_index_of("\\x05\\x80\\x80\\x80", 4, m_m4aHdr.sizeof_esds);
        if (dec_specific_offset >= 0 && (uint32_t)(dec_specific_offset + 5) <= m_m4aHdr.sizeof_esds) { // decoderSpecificInfo found
            uint8_t dec_specific_length = ((uint8_t*)esds_buffer.get())[dec_specific_offset + 4]; // Länge nach Tag + 3 Extended Length Bytes
            AUDIO_LOG_DEBUG("DecoderSpecificInfo found at offset {}, length: {}", dec_specific_offset, dec_specific_length);
            if (dec_specific_length < 2 || (uint32_t)(dec_specific_offset + 5 + dec_specific_length) > m_m4aHdr.sizeof_esds) {
                AUDIO_LOG_ERROR("invalid DecoderSpecificInfo length {}", dec_specific_length);
                stopSong();
                return 0;
            }

            // extract decoderSpecificInfo-data
            ps_ptr<char> dec_specific_data;
            dec_specific_data.alloc(dec_specific_length, "dec_specific_data");""",
        ),
        (
            "m4a-ilst-bounds",
            """        while (true) {
            atom_size.big_endian(data + consumed, 4);
            atom_name.copy_from(data + 4 + consumed, 4);
            uint32_t as = atom_size.to_uint32(16);
            AUDIO_LOG_DEBUG("atom {} @ {}, size: {}, ends @ {}", atom_name.c_get(), pos, as, pos + as);
            m_m4aHdr.ilst_pos += as;
            pos += as;
            consumed += as;

            // 0x00, 0x00, 0x00, 0x1C, 0xA9, 0x64, 0x61, 0x79, 0x00, 0x00, 0x00, 0x14, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x32, 0x30, 0x32, 0x32,
            //     sub atom length   |   ©    d     a     y  |  sub sub atom length  |  d     a     t     a  | data type   1->UTF-8  |   reserved            |  2     0     2     2

            ps_ptr<char> sa; // sub atom
            sa.copy_from(data + consumed - as, min(as, (uint32_t)1024));

            char san[5] = {0};  // sub atom name
            char ssan[5] = {0}; // sub sub atom name (should be 'data')
            strncpy(san, &sa[4], 4);
            strncpy(ssan, &sa[12], 4);
            uint32_t ssal = bigEndian((uint8_t*)&sa[8], 4); // sub sub atom length
            uint32_t dty = bigEndian((uint8_t*)&sa[16], 4); // data type 1-UTF8
            if (strncmp(ssan, "data", 4) == 0) {""",
            """        while (true) {
            if (consumed + 8 > len) {
                if (consumed > 0) {
                    m_m4aHdr.ilst_already_consumed += consumed;
                    m_m4aHdr.retvalue = consumed;
                    m_m4aHdr.headerSize += consumed;
                    m_controlCounter = M4A_ILST;
                    return 0;
                }
                m_m4aHdr.ilst_pos = m_m4aHdr.sizeof_ilst;
                break;
            }
            atom_size.big_endian(data + consumed, 4);
            atom_name.copy_from(data + 4 + consumed, 4);
            uint32_t as = atom_size.to_uint32(16);
            if (as < 8) {
                AUDIO_LOG_WARN("invalid M4A ILST atom size {}", as);
                m_m4aHdr.ilst_pos = m_m4aHdr.sizeof_ilst;
                break;
            }
            AUDIO_LOG_DEBUG("atom {} @ {}, size: {}, ends @ {}", atom_name.c_get(), pos, as, pos + as);
            size_t atomStart = consumed;
            m_m4aHdr.ilst_pos += as;
            pos += as;
            consumed += as;

            // 0x00, 0x00, 0x00, 0x1C, 0xA9, 0x64, 0x61, 0x79, 0x00, 0x00, 0x00, 0x14, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x32, 0x30, 0x32, 0x32,
            //     sub atom length   |   ©    d     a     y  |  sub sub atom length  |  d     a     t     a  | data type   1->UTF-8  |   reserved            |  2     0     2     2

            ps_ptr<char> sa; // sub atom
            size_t avail = (atomStart < len) ? min((size_t)min(as, (uint32_t)1024), len - atomStart) : 0;
            char san[5] = {0};  // sub atom name
            char ssan[5] = {0}; // sub sub atom name (should be 'data')
            uint32_t ssal = 0;
            uint32_t dty = 0;
            if (avail >= 25) {
                sa.copy_from(data + atomStart, avail);
                strncpy(san, &sa[4], 4);
                strncpy(ssan, &sa[12], 4);
                ssal = bigEndian((uint8_t*)&sa[8], 4); // sub sub atom length
                dty = bigEndian((uint8_t*)&sa[16], 4); // data type 1-UTF8
            }
            if (avail >= 25 && strncmp(ssan, "data", 4) == 0) {""",
        ),
    ]
    changed = any(apply_idempotent_patches(p, patches) for p in paths)
    print(
        f"patchAudioLib: validated ESP32-audioI2S v4 ({'patched' if changed else 'up-to-date'})"
    )


def patch_a2dp_library() -> None:
    paths = find_lib_files("ESP32-A2DP", "src/BluetoothA2DPSource.cpp", required=False)
    if not paths:
        return
    patches = [
        (
            "avrc-tg-param-sizeof",
            """      bt_app_work_dispatch(ccall_av_hdl_avrc_tg_evt, event, param,
                           sizeof(esp_a2d_cb_param_t), nullptr);""",
            """      bt_app_work_dispatch(ccall_av_hdl_avrc_tg_evt, event, param,
                           sizeof(esp_avrc_tg_cb_param_t), nullptr);""",
        ),
    ]
    if any(apply_idempotent_patches(p, patches) for p in paths):
        print("patchAudioLib: patched ESP32-A2DP AVRCP TG callback param size")


def patch_asynctcp_library() -> None:
    paths = find_lib_files("AsyncTCP", "src/AsyncTCP.cpp")
    patches = [
        (
            "asynctcp-service-hook",
            """static void _async_service_task(void *pvParameters) {
  async_tcp_log_d("Task 'async_tcp' started on core %d", static_cast<int>(xPortGetCoreID()));
#if CONFIG_ASYNC_TCP_USE_WDT
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    async_tcp_log_w("Failed to add async task to WDT");
  }
#endif
  for (;;) {
    while (auto packet = _get_async_event()) {
      async_tcp_log_elapsed("handle_async_event", AsyncTCP_detail::handle_async_event(packet));
#if CONFIG_ASYNC_TCP_USE_WDT
      esp_task_wdt_reset();
#endif
    }
    // queue is empty""",
            """extern "C" __attribute__((weak)) void espuino_async_tcp_service_hook(void) {}
extern "C" TaskHandle_t espuino_async_tcp_task_handle(void) {
  return _async_service_task_handle;
}
extern "C" void espuino_async_tcp_wake(void) {
  TaskHandle_t h = _async_service_task_handle;
  if (h) {
    xTaskNotifyGive(h);
  }
}

static void _async_service_task(void *pvParameters) {
  async_tcp_log_d("Task 'async_tcp' started on core %d", static_cast<int>(xPortGetCoreID()));
#if CONFIG_ASYNC_TCP_USE_WDT
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    async_tcp_log_w("Failed to add async task to WDT");
  }
#endif
  for (;;) {
    while (auto packet = _get_async_event()) {
      async_tcp_log_elapsed("handle_async_event", AsyncTCP_detail::handle_async_event(packet));
#if CONFIG_ASYNC_TCP_USE_WDT
      esp_task_wdt_reset();
#endif
    }
    espuino_async_tcp_service_hook();
#if CONFIG_ASYNC_TCP_USE_WDT
    esp_task_wdt_reset();
#endif
    // queue is empty""",
        ),
    ]
    if any(apply_idempotent_patches(p, patches) for p in paths):
        print("patchAudioLib: patched AsyncTCP single-task service hook")


def patch_asyncwebserver_library() -> None:
    paths = find_lib_files("ESPAsyncWebServer", "src/AsyncWebSocket.cpp")
    patches = [
        (
            "ws-disconnect-reason-bounds",
            """        if (datalen) {
          uint16_t reasonCode = (uint16_t)(data[0] << 8) + data[1];
          char *reasonString = (char *)(data + 2);
          if (reasonCode > 1001) {
            _server->_handleEvent(this, WS_EVT_ERROR, (void *)&reasonCode, (uint8_t *)reasonString, strlen(reasonString));
          }
        }""",
            """        if (datalen >= 2) {
          uint16_t reasonCode = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
          const uint8_t *reasonBytes = (datalen > 2) ? (data + 2) : (const uint8_t *)"";
          size_t reasonLen = datalen - 2;
          if (reasonCode > 1001) {
            _server->_handleEvent(this, WS_EVT_ERROR, (void *)&reasonCode, (uint8_t *)reasonBytes, reasonLen);
          }
        }""",
        ),
    ]
    if any(apply_idempotent_patches(p, patches) for p in paths):
        print("patchAudioLib: patched ESPAsyncWebServer WS_DISCONNECT bounds")


def patch_homespan_library() -> None:
    paths = find_lib_files("HomeSpan", "src/HomeSpan.cpp", required=False)
    if not paths:
        return
    patches = [
        (
            "homespan-no-early-wifi-start",
            """  WiFi.setAutoReconnect(false);                           // allow HomeSpan to handle disconnect/reconnect logic
  WiFi.persistent(false);                                 // do not permanently store WiFi configuration data
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);              // scan ALL channels - do NOT stop at first SSID match, else you could connect to weaker BSSID
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);          // sort scan data by RSSI and connect to strongest BSSID with matching SSID

  WiFi.mode(WIFI_STA);                                    // start up in STA mode, which enables WiFi radio to provide entropy source for random numbers
  delay(1);                                               // required to yield (prevents crashing)""",
            """  // patched: do not start WiFi radio in pre-setup init() before peripheral rail/SD bring-up or in BT/offline/QEMU modes""",
        ),
    ]
    if any(apply_idempotent_patches(p, patches) for p in paths):
        print("patchAudioLib: patched HomeSpan pre-setup WiFi radio start")


validate_and_patch_audio_library()
patch_a2dp_library()
patch_asynctcp_library()
patch_asyncwebserver_library()
patch_homespan_library()
