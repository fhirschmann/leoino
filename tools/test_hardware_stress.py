#!/usr/bin/env python3
"""Multi-subsystem hardware stress & burn-in suite for ESPuino / AT-2 development boards.

Exercises concurrent SD_MMC multi-codec audio playback (MP3, FLAC, M4A/AAC, WAV, M3U) plus
adversarial parser inputs, NVS flash writes + Audio::setTone + volume ramps under active
dual-core ISRs, multipart SD uploads + mid-transfer TCP disconnect recovery, WebDAV, FTP,
WebSocket broadcasts + malformed WS_DISCONNECT close frames, and SafeJsonWebHandler fuzzing.

Usage:
  ESPUINO_IP=<device-ip> python3 tools/test_hardware_stress.py --duration 240 --serial /dev/ttyUSB-espuino
"""

import argparse
import base64
import ftplib
import hashlib
import http.client
import io
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

TEST_DIR = "/__hw_stress__"

PANIC_MARKERS = (
    "Guru Meditation",
    "Backtrace:",
    "CORRUPT HEAP",
    "insert_free_block",
    "Task watchdog got triggered",
    "Brownout detector",
    "Stack canary watchpoint",
    "assert failed:",
    "abort() was called",
    "rst:0x",
)


class StressClient:
    def __init__(self, host, http_port=80, webdav_port=81, ftp_port=21, api_key=""):
        self.host = host
        self.http_port = http_port
        self.webdav_port = webdav_port
        self.ftp_port = ftp_port
        self.api_key = api_key

    def http_req(self, method, path, body=None, headers=None, port=None, timeout=12):
        hdrs = dict(headers or {})
        if self.api_key and (port is None or port == self.http_port):
            hdrs.setdefault("X-API-Key", self.api_key)
        conn = http.client.HTTPConnection(self.host, port or self.http_port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=hdrs)
            resp = conn.getresponse()
            data = resp.read()
            return resp.status, data
        finally:
            conn.close()

    def ws_session(self, messages, close_payload=b"\x03\xe8", timeout=6):
        s = socket.create_connection((self.host, self.http_port), timeout=timeout)
        try:
            key = base64.b64encode(os.urandom(16)).decode()
            auth_hdr = f"X-API-Key: {self.api_key}\r\n" if self.api_key else ""
            req = (
                f"GET /ws HTTP/1.1\r\n"
                f"Host: {self.host}\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n"
                f"{auth_hdr}\r\n"
            ).encode()
            s.sendall(req)
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(1024)
                if not chunk:
                    return False
                buf += chunk
            if b"101" not in buf.split(b"\r\n", 1)[0]:
                return False

            def send_frame(opcode, payload):
                mask = os.urandom(4)
                n = len(payload)
                hdr = bytearray([0x80 | opcode])
                if n < 126:
                    hdr.append(0x80 | n)
                elif n < 65536:
                    hdr.append(0x80 | 126)
                    hdr.extend(struct.pack("!H", n))
                else:
                    hdr.append(0x80 | 127)
                    hdr.extend(struct.pack("!Q", n))
                hdr.extend(mask)
                masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                s.sendall(bytes(hdr) + masked)

            for msg in messages:
                send_frame(0x1, msg if isinstance(msg, bytes) else msg.encode())
                time.sleep(0.02)

            if close_payload is not None:
                send_frame(0x8, close_payload)
                s.settimeout(0.3)
                try:
                    s.recv(512)
                except Exception:
                    pass
            return True
        finally:
            try:
                s.close()
            except Exception:
                pass

    def multipart_upload(self, dest_dir, filename, content_bytes, timeout=30):
        boundary = "----ESPuinoStressBoundary" + os.urandom(6).hex()
        parts = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + content_bytes + f"\r\n--{boundary}--\r\n".encode()
        q = urllib.parse.quote(dest_dir, safe="/")
        return self.http_req(
            "POST",
            f"/explorer?path={q}",
            body=parts,
            headers={
                "Content-Type": f"multipart/form-data; boundary={boundary}",
                "Content-Length": str(len(parts)),
            },
            timeout=timeout,
        )


def generate_media_assets(workdir):
    assets = {}
    cover_jpg = os.path.join(workdir, "cover.jpg")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "color=c=blue:s=64x64:d=1",
            "-frames:v", "1", cover_jpg,
        ],
        check=True,
    )

    mp3_path = os.path.join(workdir, "tone.mp3")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=8",
            "-i", cover_jpg,
            "-map", "0:a", "-map", "1:v",
            "-c:a", "libmp3lame", "-b:a", "128k",
            "-c:v", "mjpeg", "-id3v2_version", "3",
            "-metadata", "title=StressMP3", "-metadata", "artist=Audit",
            mp3_path,
        ],
        check=True,
    )
    assets["tone.mp3"] = open(mp3_path, "rb").read()

    flac_path = os.path.join(workdir, "tone.flac")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "sine=frequency=554:sample_rate=44100:duration=8",
            "-c:a", "flac", "-metadata", "title=StressFLAC",
            flac_path,
        ],
        check=True,
    )
    assets["tone.flac"] = open(flac_path, "rb").read()

    m4a_path = os.path.join(workdir, "tone.m4a")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "sine=frequency=659:sample_rate=44100:duration=8",
            "-c:a", "aac", "-b:a", "96k",
            "-metadata", "title=StressM4A", "-metadata", "artist=Audit",
            m4a_path,
        ],
        check=True,
    )
    assets["tone.m4a"] = open(m4a_path, "rb").read()

    wav_path = os.path.join(workdir, "tone.wav")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=22050:duration=8",
            "-c:a", "pcm_s16le",
            wav_path,
        ],
        check=True,
    )
    assets["tone.wav"] = open(wav_path, "rb").read()

    assets["mix.m3u"] = (
        "#EXTM3U\n"
        f"{TEST_DIR}/tone.mp3\n"
        f"{TEST_DIR}/tone.flac\n"
        f"{TEST_DIR}/tone.m4a\n"
        f"{TEST_DIR}/tone.wav\n"
    ).encode()

    xing_frame = bytearray(512)
    xing_frame[0:4] = b"\xff\xfb\x9c\x00"
    xing_frame[36:40] = b"Xing"
    xing_frame[40:44] = struct.pack(">I", 0x00000001)
    xing_frame[44:48] = struct.pack(">I", 100)
    assets["bad_xing.mp3"] = bytes(xing_frame)

    id3 = bytearray(b"ID3\x03\x00\x00\x00\x00\x01\x00")
    id3.extend(b"APIC" + struct.pack(">I", 0x00100000) + b"\x00\x00\x00image/jpeg\x00\x03\x00\xff\xd8")
    assets["bad_id3.mp3"] = bytes(id3)

    assets["bad_flac.flac"] = b"fLaC\x86\xff\xff\xff" + b"\x00" * 64

    ftyp = struct.pack(">I4s4sI4s", 20, b"ftyp", b"M4A ", 0, b"M4A ")
    bad_atom = struct.pack(">I4s", 0, b"\xa9nam")
    ilst = struct.pack(">I4s", 8 + len(bad_atom), b"ilst") + bad_atom
    meta = struct.pack(">I4sI", 12 + len(ilst), b"meta", 0) + ilst
    udta = struct.pack(">I4s", 8 + len(meta), b"udta") + meta
    moov = struct.pack(">I4s", 8 + len(udta), b"moov") + udta
    assets["bad_ilst.m4a"] = ftyp + moov

    assets["short.pls"] = b"[playlist]\nFile1=\nNumberOfEntries=1\n"
    return assets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("ESPUINO_IP", ""), help="Device IP or hostname")
    parser.add_argument("--api-key", default=os.environ.get("ESPUINO_API_KEY", ""), help="Optional API key / password")
    parser.add_argument("--duration", type=int, default=180, help="Stress duration in seconds")
    parser.add_argument("--serial", default="", help="Optional USB serial device for passive panic monitoring")
    args = parser.parse_args()

    if not args.host:
        parser.error("Provide --host or set ESPUINO_IP")

    client = StressClient(args.host, api_key=args.api_key)
    stop_evt = threading.Event()
    monitor_armed = threading.Event()
    serial_panics = []
    serial_lines = []
    ser_thread = None

    if args.serial:
        import serial as pyserial

        def serial_watcher():
            try:
                subprocess.run(["stty", "-F", args.serial, "-hupcl"], check=False)
                ser = pyserial.Serial()
                ser.port = args.serial
                ser.baudrate = 115200
                ser.timeout = 0.25
                ser.dtr = False
                ser.rts = False
                ser.open()
                while not stop_evt.is_set():
                    line = ser.readline()
                    if not line:
                        continue
                    txt = line.decode("utf-8", "replace").rstrip()
                    serial_lines.append(txt)
                    if monitor_armed.is_set():
                        for marker in PANIC_MARKERS:
                            if marker in txt:
                                serial_panics.append(txt)
                ser.close()
            except Exception as exc:
                print(f"[SERIAL] warning: {exc}")

        ser_thread = threading.Thread(target=serial_watcher, daemon=True)
        ser_thread.start()
        time.sleep(6.5)

    dbg0 = None
    for _ in range(20):
        try:
            st, raw_dbg0 = client.http_req("GET", "/debug", timeout=5)
            if st == 200:
                dbg0 = json.loads(raw_dbg0)
                break
        except Exception:
            pass
        time.sleep(1.0)
    assert dbg0 is not None, f"Device {args.host} unreachable on /debug!"
    monitor_armed.set()

    uptime0 = dbg0["runtime"]["uptimeMs"]
    orig_vol = dbg0.get("volume", 5)
    assert not dbg0["coredump"]["available"], "Pre-existing coredump detected on target!"

    st, settings_raw = client.http_req("GET", "/settings")
    assert st == 200
    settings0 = json.loads(settings_raw)
    orig_eq = settings0.get("equalizer", {"gainLowPass": 0, "gainBandPass": 0, "gainHighPass": 0})
    orig_ftp_running = bool(settings0.get("ftp", {}).get("enable", False))
    orig_dav_running = bool(settings0.get("webdav", {}).get("running", False))

    st, root0_raw = client.http_req("GET", "/explorer?path=/")
    assert st == 200
    root0_names = sorted(e.get("name", "") for e in json.loads(root0_raw))

    stats = {
        "http_ok": 0,
        "http_err": 0,
        "ws_sessions": 0,
        "ws_malformed_close": 0,
        "nvs_eq_writes": 0,
        "vol_changes": 0,
        "audio_switches": 0,
        "seeks": 0,
        "cover_reqs": 0,
        "chunked_lies": 0,
        "webdav_cycles": 0,
        "webdav_bytes": 0,
        "ftp_cycles": 0,
        "ftp_bytes": 0,
        "upload_aborts": 0,
        "min_free_heap": dbg0["freeHeap"],
        "min_largest_block": dbg0["largestFreeInternalBlock"],
        "min_stacks": {},
    }
    stats_lock = threading.Lock()
    fatal_errors = []

    def bump(key, delta=1):
        with stats_lock:
            stats[key] += delta

    workdir = tempfile.mkdtemp(prefix="espuino_hw_stress_")
    try:
        assets = generate_media_assets(workdir)
        client.ws_session(['{"webdavStatus":{"start":1}}', '{"ftpStatus":{"start":1}}'])
        time.sleep(0.8)

        client.http_req("DELETE", TEST_DIR, port=client.webdav_port, timeout=15)
        st, _ = client.http_req("MKCOL", TEST_DIR, port=client.webdav_port, timeout=10)
        assert st in (201, 405), f"MKCOL {TEST_DIR} failed: {st}"
        st, _ = client.http_req("MKCOL", f"{TEST_DIR}/dav", port=client.webdav_port, timeout=10)
        assert st in (201, 405)

        for name, content in assets.items():
            st, body = client.multipart_upload(TEST_DIR, name, content)
            assert st == 200, f"Upload {name} failed: {st} {body}"

        # Mid-upload TCP disconnect abort + recovery check
        big_payload = os.urandom(196608)
        boundary = "----AbortBoundary12345"
        part_hdr = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="aborted.bin"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode()
        total_len = len(part_hdr) + len(big_payload) + len(f"\r\n--{boundary}--\r\n")
        auth_hdr = f"X-API-Key: {args.api_key}\r\n" if args.api_key else ""
        s = socket.create_connection((args.host, client.http_port), timeout=5)
        s.sendall(
            f"POST /explorer?path={TEST_DIR} HTTP/1.1\r\n"
            f"Host: {args.host}\r\n"
            f"{auth_hdr}"
            f"Content-Type: multipart/form-data; boundary={boundary}\r\n"
            f"Content-Length: {total_len}\r\n\r\n".encode()
            + part_hdr
            + big_payload[:49152]
        )
        s.close()
        bump("upload_aborts")
        time.sleep(0.8)
        st, _ = client.multipart_upload(TEST_DIR, "after_abort.bin", os.urandom(32768))
        assert st == 200, f"Post-abort upload failed: {st}"

        deadline = time.monotonic() + args.duration

        def audio_worker():
            targets = [
                (f"{TEST_DIR}/mix.m3u", 8),
                (f"{TEST_DIR}/tone.mp3", 1),
                (f"{TEST_DIR}/tone.flac", 1),
                (f"{TEST_DIR}/tone.m4a", 1),
                (f"{TEST_DIR}/tone.wav", 1),
                (f"{TEST_DIR}", 3),
                (f"{TEST_DIR}/bad_xing.mp3", 1),
                (f"{TEST_DIR}/bad_ilst.m4a", 1),
                (f"{TEST_DIR}/short.pls", 8),
            ]
            idx = 0
            while time.monotonic() < deadline and not fatal_errors:
                path, mode = targets[idx % len(targets)]
                idx += 1
                try:
                    qpath = urllib.parse.quote(path, safe="/")
                    st, _ = client.http_req("POST", f"/exploreraudio?path={qpath}&playmode={mode}", timeout=8)
                    if st == 200:
                        bump("audio_switches")
                    is_valid = "bad_" not in path and "short." not in path
                    for step in range(5 if is_valid else 1):
                        if time.monotonic() >= deadline:
                            break
                        time.sleep(0.55)
                        if is_valid:
                            pos = (step * 23 + 15) % 90
                            client.ws_session([
                                json.dumps({"trackProgress": {"posPercent": pos}}),
                                '{"trackinfo":{}}',
                            ])
                            bump("seeks")
                            if mode in (3, 8) and step == 2:
                                client.ws_session(['{"controls":{"action":171}}'])
                            st_c, _ = client.http_req("GET", "/cover", timeout=6)
                            if st_c in (200, 204, 404):
                                bump("cover_reqs")
                except Exception:
                    bump("http_err")

        def nvs_volume_worker():
            eq_seq = [(-2, 1, -1), (2, -1, 2), (0, 2, -2), (-1, -2, 1), (0, 0, 0)]
            i = 0
            while time.monotonic() < deadline and not fatal_errors:
                try:
                    lo, mid, hi = eq_seq[i % len(eq_seq)]
                    i += 1
                    payload = json.dumps({
                        "equalizer": {"gainLowPass": lo, "gainBandPass": mid, "gainHighPass": hi}
                    }).encode()
                    st, _ = client.http_req(
                        "POST", "/settings", body=payload,
                        headers={"Content-Type": "application/json"}, timeout=10,
                    )
                    if st == 200:
                        bump("nvs_eq_writes")
                    else:
                        bump("http_err")
                    vols = [f'{{"controls":{{"set_volume":{v}}}}}' for v in (2, 4, 6, 5, 3, 1, 4, 5)]
                    if client.ws_session(vols):
                        bump("vol_changes", len(vols))
                        bump("ws_sessions")
                    time.sleep(0.35)
                except Exception:
                    bump("http_err")
                    time.sleep(0.3)

        def ws_fuzzer_worker():
            bad_closes = [b"", b"\x03", b"\x03\xe8", b"\x03\xe8" + b"X" * 55]
            k = 0
            while time.monotonic() < deadline and not fatal_errors:
                try:
                    close_pl = bad_closes[k % len(bad_closes)]
                    k += 1
                    if client.ws_session(['{"ping":{}}', '{"volume":{}}', '{"trackinfo":{}}', '{"coverimg":{}}'], close_payload=close_pl):
                        bump("ws_sessions")
                        bump("ws_malformed_close")
                    time.sleep(0.15)
                except Exception:
                    time.sleep(0.2)

        def http_telemetry_fuzzer():
            last_dbg = 0.0
            n = 0
            while time.monotonic() < deadline and not fatal_errors:
                now = time.monotonic()
                n += 1
                try:
                    if now - last_dbg >= 5.0:
                        last_dbg = now
                        st, raw = client.http_req("GET", "/debug", timeout=10)
                        if st == 200:
                            bump("http_ok")
                            d = json.loads(raw)
                            if d["runtime"]["uptimeMs"] < uptime0:
                                fatal_errors.append(f"REBOOT DETECTED: {d['runtime']['uptimeMs']} < {uptime0}")
                                return
                            if d["coredump"]["available"]:
                                fatal_errors.append("COREDUMP DETECTED during run")
                                return
                            with stats_lock:
                                stats["min_free_heap"] = min(stats["min_free_heap"], d["freeHeap"], d["minFreeHeap"])
                                stats["min_largest_block"] = min(stats["min_largest_block"], d["largestFreeInternalBlock"])
                                for t in d.get("tasks", {}).get("tasksList", []):
                                    tname = t["name"]
                                    hwm = t["stackHighWaterMark"]
                                    stats["min_stacks"][tname] = min(stats["min_stacks"].get(tname, hwm), hwm)

                    choice = n % 7
                    if choice == 0:
                        st, _ = client.http_req("GET", "/info", timeout=8)
                    elif choice == 1:
                        st, _ = client.http_req("GET", "/stats", timeout=8)
                    elif choice == 2:
                        st, _ = client.http_req("GET", "/playstats", timeout=8)
                    elif choice == 3:
                        st, _ = client.http_req("GET", f"/explorer?path={TEST_DIR}", timeout=8)
                    elif choice == 4:
                        big_json = ('{"unknownKey":"' + ("A" * 10000) + '"}').encode()
                        chunks = (
                            f"{len(big_json)//2:x}\r\n".encode() + big_json[:len(big_json)//2] + b"\r\n"
                            + f"{len(big_json) - len(big_json)//2:x}\r\n".encode() + big_json[len(big_json)//2:] + b"\r\n0\r\n\r\n"
                        )
                        st, _ = client.http_req(
                            "POST", "/settings", body=chunks,
                            headers={
                                "Content-Type": "application/json",
                                "Transfer-Encoding": "chunked",
                                "X-Expected-Entity-Length": "8",
                            },
                            timeout=10,
                        )
                        bump("chunked_lies")
                    elif choice == 5:
                        huge = b'{"x":"' + (b"B" * 36000) + b'"}'
                        st, _ = client.http_req("POST", "/settings", body=huge, headers={"Content-Type": "application/json"}, timeout=10)
                    else:
                        st, _ = client.http_req(
                            "POST", "/bluetoothconnect",
                            body=b'{"address":"ZZ:ZZ:ZZ","name":"bad"}',
                            headers={"Content-Type": "application/json"}, timeout=8,
                        )
                    if st in (200, 204, 400, 404, 405, 413):
                        bump("http_ok")
                    else:
                        bump("http_err")
                    time.sleep(0.18)
                except Exception:
                    bump("http_err")
                    time.sleep(0.25)

        def webdav_worker():
            seq = 0
            while time.monotonic() < deadline and not fatal_errors:
                seq += 1
                try:
                    blob = os.urandom(65536)
                    sha = hashlib.sha256(blob).hexdigest()
                    p1 = f"{TEST_DIR}/dav/item_{seq % 4}.bin"
                    p2 = f"{TEST_DIR}/dav/moved_{seq % 4}.bin"
                    st, _ = client.http_req("PUT", p1, body=blob, port=client.webdav_port, timeout=15)
                    if st not in (200, 201, 204):
                        bump("http_err")
                        time.sleep(0.5)
                        continue
                    bump("webdav_bytes", len(blob))
                    st, _ = client.http_req("PROPFIND", f"{TEST_DIR}/dav", headers={"Depth": "1"}, port=client.webdav_port, timeout=10)
                    assert st == 207
                    st, got = client.http_req("GET", p1, port=client.webdav_port, timeout=15)
                    assert st == 200 and hashlib.sha256(got).hexdigest() == sha
                    bump("webdav_bytes", len(got))
                    st, _ = client.http_req(
                        "MOVE", p1,
                        headers={"Destination": f"http://{args.host}:{client.webdav_port}{p2}", "Overwrite": "T"},
                        port=client.webdav_port, timeout=10,
                    )
                    assert st in (201, 204)
                    st, _ = client.http_req("DELETE", p2, port=client.webdav_port, timeout=10)
                    assert st in (200, 204)
                    st_prot, _ = client.http_req("DELETE", "/Playlists", port=client.webdav_port, timeout=8)
                    assert st_prot == 403
                    bump("webdav_cycles")
                    time.sleep(0.4)
                except Exception:
                    bump("http_err")
                    time.sleep(0.5)

        def ftp_worker():
            seq = 0
            while time.monotonic() < deadline and not fatal_errors:
                seq += 1
                try:
                    blob = os.urandom(65536)
                    sha = hashlib.sha256(blob).hexdigest()
                    ftp = ftplib.FTP()
                    ftp.connect(args.host, client.ftp_port, timeout=10)
                    ftp.login("esp32", args.api_key or "esp32")
                    ftp.cwd(f"/SD-Card{TEST_DIR}")
                    lines = []
                    ftp.retrlines("MLSD", lines.append)
                    fname1 = f"ftp_{seq % 4}.bin"
                    fname2 = f"ftp_ren_{seq % 4}.bin"
                    ftp.storbinary(f"STOR {fname1}", io.BytesIO(blob), blocksize=8192)
                    bump("ftp_bytes", len(blob))
                    out = io.BytesIO()
                    ftp.retrbinary(f"RETR {fname1}", out.write, blocksize=8192)
                    assert hashlib.sha256(out.getvalue()).hexdigest() == sha
                    bump("ftp_bytes", len(blob))
                    ftp.rename(fname1, fname2)
                    ftp.delete(fname2)
                    ftp.quit()
                    bump("ftp_cycles")
                    time.sleep(0.5)
                except Exception:
                    bump("http_err")
                    time.sleep(0.6)

        workers = [
            threading.Thread(target=audio_worker),
            threading.Thread(target=nvs_volume_worker),
            threading.Thread(target=ws_fuzzer_worker),
            threading.Thread(target=http_telemetry_fuzzer),
            threading.Thread(target=webdav_worker),
            threading.Thread(target=ftp_worker),
        ]
        for w in workers:
            w.start()
        for w in workers:
            w.join()

    finally:
        try:
            client.ws_session([
                '{"controls":{"action":173}}',
                f'{{"controls":{{"set_volume":{orig_vol}}}}}',
                f'{{"ftpStatus":{{"start":{1 if orig_ftp_running else 0}}}}}',
                f'{{"webdavStatus":{{"start":{1 if orig_dav_running else 0}}}}}',
            ])
            time.sleep(0.5)
            client.http_req(
                "POST", "/settings",
                body=json.dumps({
                    "equalizer": {
                        "gainLowPass": orig_eq.get("gainLowPass", 0),
                        "gainBandPass": orig_eq.get("gainBandPass", 0),
                        "gainHighPass": orig_eq.get("gainHighPass", 0),
                    }
                }).encode(),
                headers={"Content-Type": "application/json"},
                timeout=10,
            )
            client.http_req("DELETE", f"/explorer?path={TEST_DIR}", timeout=20)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)
            time.sleep(1.0)

    st, raw_dbg1 = client.http_req("GET", "/debug", timeout=10)
    assert st == 200
    dbg1 = json.loads(raw_dbg1)
    uptime1 = dbg1["runtime"]["uptimeMs"]

    st, root1_raw = client.http_req("GET", "/explorer?path=/", timeout=10)
    assert st == 200
    root1_names = sorted(e.get("name", "") for e in json.loads(root1_raw))

    stop_evt.set()
    if ser_thread:
        ser_thread.join(timeout=2.0)

    print(json.dumps({
        "uptimeBeforeMs": uptime0,
        "uptimeAfterMs": uptime1,
        "freeHeap": dbg1["freeHeap"],
        "minFreeHeap": dbg1["minFreeHeap"],
        "largestFreeInternalBlock": dbg1["largestFreeInternalBlock"],
        "coredump": dbg1["coredump"]["available"],
        "serialPanics": len(serial_panics),
        "sdRootClean": root1_names == root0_names,
        "stats": stats,
    }, indent=2))

    if serial_panics or fatal_errors or uptime1 <= uptime0 or dbg1["coredump"]["available"] or TEST_DIR.strip("/") in root1_names:
        sys.exit(1)


if __name__ == "__main__":
    main()
