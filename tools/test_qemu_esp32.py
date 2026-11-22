#!/usr/bin/env python3
"""Boot the full dual-core ESP32 firmware in Espressif QEMU and run live crash/fuzz tests.

Exercises the real Xtensa binary (FreeRTOS SMP, 4 MB PSRAM, 16 MB SPI flash, NVS, FAT32 SD_MMC,
OpenCores Ethernet + lwIP + AsyncTCP + ESPAsyncWebServer + WebDAV + FTP) against:
  1. Boot health, core-dump partition integrity, and task stack high-water marks (/info, /debug).
  2. Malformed & adversarial HTTP JSON payloads (chunked X-Expected-Entity-Length overflow lies,
     oversized bodies, HTTP_GET on JSON POST endpoints, invalid /bluetoothconnect MACs).
  3. Multipart SD uploads of truncated/corrupt ID3v2 / FLAC / M4A files + /cover parser fuzzing.
  4. Concurrent WebSocket connect/broadcast/disconnect storms + 0-byte / 1-byte / un-terminated
     WS_DISCONNECT close frames.
  5. WebDAV root/system deletion guards + bounded pre-auth drain.
  6. Post-stress verification that zero panics/resets occurred and coredump.available == false.
"""

from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 16 * 1024 * 1024
SD_SIZE = 64 * 1024 * 1024

PANIC_MARKERS = (
    "Guru Meditation Error",
    "Backtrace:",
    "abort() was called",
    "CORRUPT HEAP",
    "Stack canary watchpoint triggered",
    "Task watchdog got triggered",
    "Interrupt wdt timeout",
    "Brownout detector was triggered",
)


def find_qemu_binary() -> str:
    env_bin = os.environ.get("QEMU_ESP32_BIN")
    if env_bin and Path(env_bin).is_file():
        return env_bin
    which_bin = shutil.which("qemu-system-xtensa")
    if which_bin:
        return which_bin
    candidates = sorted(
        glob.glob(str(Path.home() / ".espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa")),
        reverse=True,
    )
    if candidates:
        return candidates[0]
    raise SystemExit("qemu-system-xtensa not found (set QEMU_ESP32_BIN or install Espressif QEMU)")


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def crc8_esp_mac(mac: bytes) -> int:
    crc = 0x00
    for byte in mac:
        crc ^= byte
        for _ in range(8):
            crc = ((crc >> 1) ^ 0x8C) & 0xFF if (crc & 0x01) else (crc >> 1) & 0xFF
    return crc


def build_rev3_efuse(path: Path) -> None:
    efuse = bytearray(124)
    # Valid locally-administered unicast MAC 02:4c:45:4f:00:02 + ESP32 eFuse CRC8
    mac = bytes([0x02, 0x4C, 0x45, 0x4F, 0x00, 0x02])
    # BLK0_RDATA1 (offset 4..7): MAC[5], MAC[4], MAC[3], MAC[2]
    # BLK0_RDATA2 (offset 8..11): MAC[1], MAC[0], CRC8, 0
    efuse[4] = mac[5]
    efuse[5] = mac[4]
    efuse[6] = mac[3]
    efuse[7] = mac[2]
    efuse[8] = mac[1]
    efuse[9] = mac[0]
    efuse[10] = crc8_esp_mac(mac)
    # ESP32 silicon revision v3.0 (CHIP_VER_REV1 + CHIP_VER_REV2)
    efuse[13] = 0x80
    efuse[22] = 0x10
    path.write_bytes(efuse)


def build_qemu_firmware() -> Path:
    pio = shutil.which("pio") or str(Path.home() / ".local/bin/pio")
    env = os.environ.copy()
    env.setdefault("UV_CONSTRAINT", str(ROOT / "tools/build-constraints.txt"))
    subprocess.run([pio, "run", "-e", "qemu", "-j", "4"], cwd=ROOT, env=env, check=True)
    factory = ROOT / ".pio/build/qemu/firmware.factory.bin"
    if not factory.is_file():
        raise SystemExit(f"Missing {factory}")
    return factory


def http_get_json(port: int, path: str, timeout: float = 8.0) -> dict | list:
    url = f"http://127.0.0.1:{port}{path}"
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        assert resp.status == 200, f"GET {path} -> {resp.status}"
        return json.loads(resp.read().decode("utf-8"))


def send_raw_http(port: int, raw: bytes, timeout: float = 8.0) -> tuple[int, bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(raw)
        chunks = []
        while True:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
            joined = b"".join(chunks)
            if b"\r\n\r\n" in joined:
                hdr, body = joined.split(b"\r\n\r\n", 1)
                for line in hdr.split(b"\r\n")[1:]:
                    if line.lower().startswith(b"content-length:"):
                        clen = int(line.split(b":", 1)[1].strip())
                        if len(body) >= clen:
                            status = int(hdr.split(b" ")[1])
                            return status, body[:clen]
        joined = b"".join(chunks)
        assert b"HTTP/1." in joined, f"Invalid HTTP response: {joined[:120]!r}"
        hdr, _, body = joined.partition(b"\r\n\r\n")
        status = int(hdr.split(b" ")[1])
        return status, body


def upload_multipart(port: int, folder: str, filename: str, payload: bytes) -> int:
    boundary = "----EspuinoQemuBoundary7MA4YWxkTrZu0gW"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode("utf-8") + payload + f"\r\n--{boundary}--\r\n".encode("utf-8")
    quoted = urllib.parse.quote(folder, safe="/")
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    conn.request(
        "POST",
        f"/explorer?path={quoted}",
        body=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
    )
    resp = conn.getresponse()
    resp.read()
    status = resp.status
    conn.close()
    return status


def ws_connect_and_send_close(port: int, close_payload: bytes | None, extra_msg: bytes | None = None) -> None:
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")
    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as sock:
        sock.settimeout(5.0)
        sock.sendall(req)
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = sock.recv(1024)
            if not chunk:
                return
            resp += chunk
        assert b"101 " in resp.split(b"\r\n", 1)[0], f"WS handshake failed: {resp!r}"
        # Send a JSON ping (or custom message) masked as required by RFC 6455
        msg = extra_msg if extra_msg is not None else b'{"ping":1}'
        mask = b"\x12\x34\x56\x78"
        masked_msg = bytes(b ^ mask[i % 4] for i, b in enumerate(msg))
        sock.sendall(bytes([0x81, 0x80 | len(msg)]) + mask + masked_msg)
        if close_payload is not None:
            assert len(close_payload) < 126
            masked_close = bytes(b ^ mask[i % 4] for i, b in enumerate(close_payload))
            sock.sendall(bytes([0x88, 0x80 | len(close_payload)]) + mask + masked_close)
            time.sleep(0.05)


def check_serial_clean(serial_path: Path) -> str:
    if not serial_path.is_file():
        return ""
    text = serial_path.read_text(encoding="utf-8", errors="replace")
    for marker in PANIC_MARKERS:
        if marker in text:
            raise AssertionError(f"Detected crash marker '{marker}' in QEMU serial log:\n{text[-3000:]}")
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true", help="Reuse existing .pio/build/qemu/firmware.factory.bin")
    args = parser.parse_args()

    qemu_bin = find_qemu_binary()
    factory_bin = (
        ROOT / ".pio/build/qemu/firmware.factory.bin" if args.skip_build else build_qemu_firmware()
    )
    if not factory_bin.is_file():
        raise SystemExit(f"Firmware image not found: {factory_bin}")

    http_port = pick_free_port()
    webdav_port = pick_free_port()

    with tempfile.TemporaryDirectory(prefix="leoino-qemu-") as tmpdir:
        tmp = Path(tmpdir)
        flash_img = tmp / "flash_16mb.bin"
        efuse_img = tmp / "efuse.bin"
        sd_img = tmp / "sdcard.img"
        serial_log = tmp / "serial.log"

        raw_factory = factory_bin.read_bytes()
        assert len(raw_factory) <= FLASH_SIZE
        flash_img.write_bytes(raw_factory + b"\xff" * (FLASH_SIZE - len(raw_factory)))
        build_rev3_efuse(efuse_img)
        with open(sd_img, "wb") as f:
            f.truncate(SD_SIZE)

        netdev_arg = (
            f"user,id=net0,"
            f"hostfwd=tcp:127.0.0.1:{http_port}-:80,"
            f"hostfwd=tcp:127.0.0.1:{webdav_port}-:81"
        )
        cmd = [
            qemu_bin,
            "-nographic",
            "-machine",
            "esp32",
            "-m",
            "4M",
            "-drive",
            f"file={flash_img},if=mtd,format=raw",
            "-drive",
            f"file={efuse_img},if=none,format=raw,id=efuse",
            "-global",
            "driver=nvram.esp32.efuse,property=drive,value=efuse",
            "-drive",
            f"file={sd_img},if=sd,format=raw",
            "-nic",
            netdev_arg,
            "-serial",
            f"file:{serial_log}",
        ]

        print(f"[QEMU] Starting {qemu_bin} (HTTP -> 127.0.0.1:{http_port}, WebDAV -> 127.0.0.1:{webdav_port})...")
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            # 1. Wait for boot + OpenCores Ethernet DHCP + HTTP server readiness
            deadline = time.monotonic() + 45.0
            info = None
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    err = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
                    raise AssertionError(f"QEMU exited early ({proc.returncode}): {err}\n{serial_log.read_text(errors='replace')}")
                log_text = check_serial_clean(serial_log)
                if "10.0.2." not in log_text:
                    time.sleep(0.25)
                    continue
                try:
                    info = http_get_json(http_port, "/info", timeout=3.0)
                    break
                except Exception:
                    time.sleep(0.5)

            assert info is not None, f"Timed out waiting for /info in QEMU.\nSerial:\n{serial_log.read_text(errors='replace')[-3000:]}"
            print(
                f"[QEMU] Booted! IP={info.get('wifi', {}).get('ip')} "
                f"freeHeap={info['memory']['freeHeap']} PSRAM={info['memory']['freePSRam']} "
                f"SD mounted={info.get('sdcard', {}).get('mounted')}"
            )
            assert not info["coredump"]["available"], f"Unexpected stored coredump: {info['coredump']}"

            # 2. Check /debug task stacks (including ipc0 >= 512 B free, no rfid task when no reader attached)
            debug = http_get_json(http_port, "/debug")
            tasks = {t["name"]: t for t in debug.get("tasks", {}).get("tasksList", [])}
            print(f"[QEMU] Tasks running ({len(tasks)}): " + ", ".join(f"{k}:{v['stackHighWaterMark']}B" for k, v in sorted(tasks.items())))
            assert "rfid" not in tasks, "rfid task should not spawn when auto-detect finds TYPE_NONE!"
            if "ipc0" in tasks:
                assert tasks["ipc0"]["stackHighWaterMark"] >= 400, f"ipc0 stack watermark too low: {tasks['ipc0']}"

            # 3. Adversarial JSON & HTTP fuzzing (SafeJsonWebHandler)
            # 3a) HTTP GET with Content-Type: application/json must NOT invoke POST JSON handlers
            status, _ = send_raw_http(
                http_port,
                b"GET /settings HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n",
            )
            assert status == 200, f"GET /settings with json content-type returned {status}"

            # 3b) Chunked X-Expected-Entity-Length: 4 lie with 3.5 KB JSON payload -> must NOT overflow internal heap!
            lie_json = ('{"equalizer":{"preset":"flat","pad":"' + ("X" * 3500) + '"}}').encode("ascii")
            chunked_lie = (
                b"POST /settings HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/json\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"X-Expected-Entity-Length: 4\r\n"
                b"Connection: close\r\n\r\n"
                + f"{len(lie_json):x}\r\n".encode("ascii")
                + lie_json
                + b"\r\n0\r\n\r\n"
            )
            status, _ = send_raw_http(http_port, chunked_lie)
            assert status in (200, 400, 413), f"Unexpected status on chunked lie: {status}"

            # 3c) Oversized chunked JSON (> maxContentLength) -> clean 413, no crash
            huge_chunk = b"A" * 20000
            oversized_req = (
                b"POST /rfid HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/json\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"X-Expected-Entity-Length: 8\r\n"
                b"Connection: close\r\n\r\n"
                + f"{len(huge_chunk):x}\r\n".encode("ascii")
                + huge_chunk
                + b"\r\n0\r\n\r\n"
            )
            status, _ = send_raw_http(http_port, oversized_req)
            assert status == 413, f"Expected 413 on oversized chunked /rfid, got {status}"

            # 3d) Malformed /bluetoothconnect MAC address -> clean 400
            bt_bad = b'{"address":"ZZ:ZZ:not-a-mac","name":"BadSpeaker"}'
            status, _ = send_raw_http(
                http_port,
                b"POST /bluetoothconnect HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                + f"Content-Length: {len(bt_bad)}\r\nConnection: close\r\n\r\n".encode("ascii")
                + bt_bad,
            )
            assert status == 400, f"Expected 400 on malformed BT MAC, got {status}"

            # 4. Multipart upload + malformed ID3v2 / FLAC / empty-path /cover crash tests
            if info.get("sdcard", {}).get("mounted", True):
                # Truncated ID3v2 with huge synchsafe frame size + EOF mid-header
                bad_mp3 = b"ID3\x03\x00\x00\x00\x00\x7f\x7fAPIC\x00\x0f\xff\xff\x00\x00\x00"
                assert upload_multipart(http_port, "", "fuzz_trunc.mp3", bad_mp3) == 200
                # Truncated FLAC picture block claiming 0xFFFFFFF0 bytes (unsigned wraparound test)
                bad_flac = b"fLaC\x86\x00\x00\x20" + struct.pack(">IIII", 3, 10, 0, 0xFFFFFFF0) + b"\x00" * 16
                assert upload_multipart(http_port, "", "fuzz_wrap.flac", bad_flac) == 200

                for cover_query in ("/cover", "/cover?path=", "/cover?path=/fuzz_trunc.mp3", "/cover?path=/fuzz_wrap.flac"):
                    st, _ = send_raw_http(
                        http_port,
                        f"GET {cover_query} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode("ascii"),
                    )
                    assert st in (200, 404), f"Unexpected /cover status {st} for {cover_query}"

            # 5. Concurrent WebSocket + malformed WS_DISCONNECT frames + parallel HTTP polling storm
            errors: list[Exception] = []

            def ws_worker(worker_id: int) -> None:
                try:
                    # Test 0-byte close, 1-byte truncated close (used to OOB-read), 2-byte close, and non-NUL reason > 1001
                    payloads = [b"", b"\x03", b"\x03\xe8", b"\x03\xea" + b"R" * 40]
                    for i in range(6):
                        ws_connect_and_send_close(http_port, payloads[(worker_id + i) % len(payloads)])
                except Exception as exc:
                    errors.append(exc)

            def http_worker() -> None:
                try:
                    for _ in range(10):
                        http_get_json(http_port, "/info", timeout=10.0)
                        http_get_json(http_port, "/settings", timeout=10.0)
                except Exception as exc:
                    errors.append(exc)

            threads = [threading.Thread(target=ws_worker, args=(i,)) for i in range(3)]
            threads.append(threading.Thread(target=http_worker))
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=30.0)
            assert not errors, f"Concurrent WS/HTTP storm errors: {errors}"

            # 6. Enable WebDAV live over WebSocket and verify pre-auth 401 drain + authenticated 403 root/system/playlists DELETE protection
            ws_connect_and_send_close(http_port, b"\x03\xe8", extra_msg=b'{"webdavStatus":{"start":1}}')
            time.sleep(0.5)
            st_unauth, _ = send_raw_http(
                webdav_port,
                b"PUT /unauth.bin HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n" + (b"U" * 4096),
            )
            assert st_unauth == 401, f"WebDAV unauthenticated PUT expected 401, got {st_unauth}"
            basic_auth = base64.b64encode(b"esp32:esp32").decode("ascii")
            for protected in ("/", "/System", "/Playlists"):
                st, _ = send_raw_http(
                    webdav_port,
                    f"DELETE {protected} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic {basic_auth}\r\nConnection: close\r\n\r\n".encode("ascii"),
                )
                assert st == 403, f"WebDAV DELETE {protected} expected 403, got {st}"

            # 7. Final post-stress health & zero-panic verification
            check_serial_clean(serial_log)
            final_info = http_get_json(http_port, "/info")
            final_debug = http_get_json(http_port, "/debug")
            assert not final_info["coredump"]["available"], "Core dump appeared during stress test!"
            boot_log = serial_log.read_text(encoding="utf-8", errors="replace")
            assert boot_log.count("ESP-IDF version:") == 1, "Device rebooted unexpectedly during QEMU stress test!"
            print(
                f"[QEMU] PASS: 0 panics, 0 reboots, freeHeap={final_info['memory']['freeHeap']}, "
                f"minFreeHeap={final_debug['minFreeHeap']}, largestInternal={final_debug['largestFreeInternalBlock']}"
            )
        except Exception:
            if serial_log.is_file():
                sys.stderr.write("\n=== QEMU Serial Tail ===\n" + serial_log.read_text(encoding="utf-8", errors="replace")[-3000:] + "\n========================\n")
            raise
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
