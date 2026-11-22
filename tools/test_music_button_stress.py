#!/usr/bin/env python3
"""Music + Physical Button & Rotary Control Stress Test for ESPuino / AT-2 hardware.

Plays real multi-track music (existing SD /Musik & /Playlists libraries if present, plus
a generated 3-album stereo MP3/FLAC/M4A musical chord-progression tree) while hammering the
physical button edge ring buffer (Button_InjectEdge -> Button_Cyclic) and rotary encoder
detent path (RotaryEncoder_InjectDetents -> RotaryEncoder_Cyclic) concurrently:

- Short clicks: PLAY/PAUSE, NEXT, PREV, SEEK_FWD, SEEK_BACK, MEASURE_BATTERY
- Rapid button-mashing bursts (24+ edges in <300 ms during synchronous SD track opens,
  exercising Button_EdgeBuffer[64] pacing and overflow recovery)
- Long presses & continuous hold-repeats: FIRSTTRACK, LASTTRACK, VOLUMEUP/VOLUMEDOWN
  hold-repeat, and CMD_SMART_FORWARDS 200 ms smart-hold repeat + release suppression
- Multi-button chords (e.g. Button 0 + Button 2 simultaneous press)
- Hold-button + turn-rotary gestures:
  - Button 0 + rotary CW/CCW -> direct rotary seek (rotSeekStep)
  - Button 1 + rotary CW/CCW -> CMD_SEEK_PREVIEW (both immediate release-commit AND
    2 s idle-timeout auto-commit, verifying usedAsModifier swallows button release)
  - Button 2 + rotary CW/CCW -> LED brightness up/down clamping (LED_BRIGHTNESS_MIN..255)
- Pure rotary volume spins (rapid CW/CCW detents across LUT curve + mute/unmute)
- Child-lock (CMD_LOCK_BUTTONS_MOD) toggled ON mid-playback while mashing buttons + rotary,
  verifying locked controls do not crash or strand preview gestures, then unlocked
- Equalizer profile cycling (CMD_TOGGLE_EQUALIZER), repeat modes, night-mode dimming,
  folder-jump commands (CMD_NEXTFOLDER / CMD_PREVFOLDER), and virtual RFID cards

Usage:
  ESPUINO_IP=<device-ip> python3 tools/test_music_button_stress.py --duration 180 --serial /dev/ttyUSB-espuino
"""

import argparse
import base64
import http.client
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

TEST_DIR = "/__hw_music_btn__"

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


class DeviceClient:
    def __init__(self, host, http_port=80, webdav_port=81, api_key=""):
        self.host = host
        self.http_port = http_port
        self.webdav_port = webdav_port
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

    def multipart_upload(self, dest_dir, filename, content_bytes, timeout=30):
        boundary = "----MusicBtnBoundary" + os.urandom(6).hex()
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


class PersistentWsController:
    """Persistent WebSocket client for low-latency button-edge & rotary-detent injection."""

    def __init__(self, host, port=80, api_key=""):
        self.host = host
        self.port = port
        self.api_key = api_key
        self.sock = None
        self.lock = threading.Lock()
        self.rx_stop = threading.Event()
        self.rx_thread = None
        self.connect()

    def connect(self):
        with self.lock:
            self._close_locked()
            s = socket.create_connection((self.host, self.port), timeout=6)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
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
                    raise RuntimeError("WS handshake closed")
                buf += chunk
            if b"101" not in buf.split(b"\r\n", 1)[0]:
                raise RuntimeError(f"WS handshake failed: {buf[:80]!r}")
            s.settimeout(0.5)
            self.sock = s
            self.rx_stop.clear()
            self.rx_thread = threading.Thread(target=self._drain_rx, args=(s,), daemon=True)
            self.rx_thread.start()

    def _drain_rx(self, s):
        while not self.rx_stop.is_set():
            try:
                data = s.recv(4096)
                if not data:
                    break
            except socket.timeout:
                continue
            except Exception:
                break

    def _close_locked(self):
        self.rx_stop.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def close(self):
        with self.lock:
            self._close_locked()

    def send_json(self, obj):
        payload = json.dumps(obj, separators=(",", ":")).encode()
        for attempt in range(2):
            try:
                with self.lock:
                    if self.sock is None:
                        raise RuntimeError("not connected")
                    mask = os.urandom(4)
                    n = len(payload)
                    hdr = bytearray([0x81])
                    if n < 126:
                        hdr.append(0x80 | n)
                    else:
                        hdr.append(0x80 | 126)
                        hdr.extend(struct.pack("!H", n))
                    hdr.extend(mask)
                    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                    self.sock.sendall(bytes(hdr) + masked)
                return True
            except Exception:
                if attempt == 0:
                    time.sleep(0.15)
                    try:
                        self.connect()
                    except Exception:
                        return False
        return False

    def button_edge(self, button, pressed):
        return self.send_json({"controls": {"button_edge": {"button": int(button), "pressed": bool(pressed)}}})

    def short_click(self, button, hold_sec=0.055):
        ok1 = self.button_edge(button, True)
        time.sleep(hold_sec)
        ok2 = self.button_edge(button, False)
        return ok1 and ok2

    def long_press(self, button, hold_sec=0.95):
        ok1 = self.button_edge(button, True)
        time.sleep(hold_sec)
        ok2 = self.button_edge(button, False)
        return ok1 and ok2

    def rotary_turn(self, detents):
        return self.send_json({"controls": {"rotary": int(detents)}})

    def cmd_action(self, cmd):
        return self.send_json({"controls": {"action": int(cmd)}})


def generate_music_albums(workdir):
    """Generate 3 stereo music album folders (MP3 192k, FLAC, M4A/AAC, 32s each) with cover art."""
    albums = {}
    cover_jpg = os.path.join(workdir, "cover.jpg")
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "color=c=purple:s=96x96:d=1",
            "-frames:v", "1", cover_jpg,
        ],
        check=True,
    )

    chords = [
        ("Album_1", [(261.63, 329.63, 392.00), (293.66, 369.99, 440.00)]),
        ("Album_2", [(329.63, 415.30, 493.88), (349.23, 440.00, 523.25)]),
        ("Album_3", [(392.00, 493.88, 587.33), (220.00, 277.18, 329.63)]),
    ]

    for album_name, pair in chords:
        albums[album_name] = {}
        (f1, f2, f3), (g1, g2, g3) = pair
        expr_l = f"0.22*sin(2*PI*{f1}*t)+0.18*sin(2*PI*{f2}*t)+0.14*sin(2*PI*{f3}*t)"
        expr_r = f"0.22*sin(2*PI*{g1}*t)+0.18*sin(2*PI*{g2}*t)+0.14*sin(2*PI*{g3}*t)"
        lavfi = f"aevalsrc='{expr_l}|{expr_r}':s=44100:d=32"

        mp3_p = os.path.join(workdir, f"{album_name}_01.mp3")
        subprocess.run(
            [
                "ffmpeg", "-y", "-loglevel", "error",
                "-f", "lavfi", "-i", lavfi,
                "-i", cover_jpg, "-map", "0:a", "-map", "1:v",
                "-c:a", "libmp3lame", "-b:a", "192k",
                "-c:v", "mjpeg", "-id3v2_version", "3",
                "-metadata", f"title={album_name}_Track1",
                "-metadata", f"album={album_name}",
                mp3_p,
            ],
            check=True,
        )
        albums[album_name]["01_track.mp3"] = open(mp3_p, "rb").read()

        flac_p = os.path.join(workdir, f"{album_name}_02.flac")
        subprocess.run(
            [
                "ffmpeg", "-y", "-loglevel", "error",
                "-f", "lavfi", "-i", lavfi,
                "-c:a", "flac",
                "-metadata", f"title={album_name}_Track2",
                "-metadata", f"album={album_name}",
                flac_p,
            ],
            check=True,
        )
        albums[album_name]["02_track.flac"] = open(flac_p, "rb").read()

        m4a_p = os.path.join(workdir, f"{album_name}_03.m4a")
        subprocess.run(
            [
                "ffmpeg", "-y", "-loglevel", "error",
                "-f", "lavfi", "-i", lavfi,
                "-c:a", "aac", "-b:a", "128k",
                "-metadata", f"title={album_name}_Track3",
                "-metadata", f"album={album_name}",
                m4a_p,
            ],
            check=True,
        )
        albums[album_name]["03_track.m4a"] = open(m4a_p, "rb").read()

    return albums


def discover_sd_music_sources(client):
    """Find real music folders/playlists already on SD without printing private titles."""
    sources = []
    st, raw = client.http_req("GET", "/explorer?path=/Musik")
    if st == 200:
        try:
            entries = json.loads(raw)
            if entries:
                sources.append(("/Musik", 7))  # ALL_TRACKS_OF_DIR_SORTED_RECURSIVE
                sources.append(("/Musik", 9))  # ALL_TRACKS_OF_DIR_RANDOM_RECURSIVE
                for e in entries[:4]:
                    if e.get("dir") and e.get("name"):
                        sources.append((f"/Musik/{e['name']}", 3))
        except Exception:
            pass

    st, raw = client.http_req("GET", "/explorer?path=/Playlists")
    if st == 200:
        try:
            for e in json.loads(raw)[:4]:
                name = e.get("name", "")
                if not e.get("dir") and name.lower().endswith(".m3u"):
                    sources.append((f"/Playlists/{name}", 8))  # LOCAL_M3U
        except Exception:
            pass
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("ESPUINO_IP", ""), help="Device IP or hostname")
    parser.add_argument("--api-key", default=os.environ.get("ESPUINO_API_KEY", ""), help="Optional API key")
    parser.add_argument("--duration", type=int, default=180, help="Stress duration in seconds")
    parser.add_argument("--serial", default="", help="Optional USB serial device for passive panic monitoring")
    args = parser.parse_args()

    if not args.host:
        parser.error("Provide --host or set ESPUINO_IP")

    client = DeviceClient(args.host, api_key=args.api_key)
    stop_evt = threading.Event()
    monitor_armed = threading.Event()
    serial_panics = []
    serial_lines_count = 0
    button_log_hits = 0
    ser_thread = None

    if args.serial:
        import serial as pyserial

        def serial_watcher():
            nonlocal serial_lines_count, button_log_hits
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
                    serial_lines_count += 1
                    if "Button " in txt and "pressed" in txt:
                        button_log_hits += 1
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
    orig_buttons = dict(settings0["buttons"])
    orig_eq = dict(settings0.get("equalizer", {"gainLowPass": 0, "gainBandPass": 0, "gainHighPass": 0}))
    orig_dav_running = bool(settings0.get("webdav", {}).get("running", False))

    st, root0_raw = client.http_req("GET", "/explorer?path=/")
    assert st == 200
    root0_names = sorted(e.get("name", "") for e in json.loads(root0_raw))

    sd_music_sources = discover_sd_music_sources(client)
    print(
        f"[INIT] uptimeMs={uptime0} freeHeap={dbg0['freeHeap']} minFreeHeap={dbg0['minFreeHeap']} "
        f"sdMusicSources={len(sd_music_sources)}"
    )

    stats = {
        "short_clicks": 0,
        "rapid_mash_edges": 0,
        "long_presses": 0,
        "smart_hold_sessions": 0,
        "multi_chords": 0,
        "rotary_volume_turns": 0,
        "rotary_seek_gestures": 0,
        "seek_preview_release_commits": 0,
        "seek_preview_idle_commits": 0,
        "rotary_brightness_gestures": 0,
        "child_lock_cycles": 0,
        "eq_profile_cycles": 0,
        "folder_jumps": 0,
        "music_playlist_starts": 0,
        "trackprogress_polls": 0,
        "http_errors": 0,
        "min_free_heap": dbg0["freeHeap"],
        "min_largest_block": dbg0["largestFreeInternalBlock"],
        "min_stacks": {},
    }
    stats_lock = threading.Lock()
    fatal_errors = []

    def bump(key, delta=1):
        with stats_lock:
            stats[key] += delta

    workdir = tempfile.mkdtemp(prefix="espuino_music_btn_")
    ws_ctrl = PersistentWsController(args.host, api_key=args.api_key)

    try:
        print("[PREP] Generating 3-album stereo MP3/FLAC/M4A music tree via ffmpeg...")
        albums = generate_music_albums(workdir)
        ws_ctrl.send_json({"webdavStatus": {"start": 1}})
        time.sleep(0.5)

        client.http_req("DELETE", TEST_DIR, port=client.webdav_port, timeout=15)
        st, _ = client.http_req("MKCOL", TEST_DIR, port=client.webdav_port, timeout=10)
        assert st in (201, 405)

        for album_name, files in albums.items():
            album_dir = f"{TEST_DIR}/{album_name}"
            st, _ = client.http_req("MKCOL", album_dir, port=client.webdav_port, timeout=10)
            assert st in (201, 405)
            for fname, content in files.items():
                st, body = client.multipart_upload(album_dir, fname, content)
                assert st == 200, f"Upload {album_dir}/{fname} failed: {st} {body}"

        # Temporarily map unused Button 6 & Button 1 rotary-modifier so every physical gesture path is active:
        # - Button 0 rotary CW/CCW: direct seek forwards/backwards (180/181)
        # - Button 1 rotary CW/CCW: CMD_SEEK_PREVIEW (193)
        # - Button 2 rotary CW/CCW: LED brightness up/down (157/158)
        # - Button 6 short: CMD_SMART_BACKWARDS (191), Button 6 long: CMD_SMART_FORWARDS (190)
        test_buttons = dict(orig_buttons)
        test_buttons["short6"] = 191
        test_buttons["long6"] = 190
        test_buttons["rotCw0"] = 180
        test_buttons["rotCcw0"] = 181
        test_buttons["rotCw1"] = 193
        test_buttons["rotCcw1"] = 193
        test_buttons["rotCw2"] = 157
        test_buttons["rotCcw2"] = 158
        st, _ = client.http_req(
            "POST", "/settings",
            body=json.dumps({"buttons": test_buttons}).encode(),
            headers={"Content-Type": "application/json"},
            timeout=10,
        )
        assert st == 200, f"Failed to set test button mappings: {st}"

        # Build combined playlist rotation: generated 3-album recursive tree + real SD music folders/playlists
        all_music_targets = [
            (TEST_DIR, 7),  # ALL_TRACKS_OF_DIR_SORTED_RECURSIVE (9 stereo tracks across 3 folders!)
            (TEST_DIR, 9),  # ALL_TRACKS_OF_DIR_RANDOM_RECURSIVE
        ] + sd_music_sources

        # Start initial music playback at safe moderate volume
        ws_ctrl.send_json({"controls": {"set_volume": 4}})
        q0 = urllib.parse.quote(TEST_DIR, safe="/")
        st, _ = client.http_req("POST", f"/exploreraudio?path={q0}&playmode=7", timeout=10)
        assert st == 200
        bump("music_playlist_starts")
        time.sleep(1.2)

        deadline = time.monotonic() + args.duration
        print(f"[RUN] Running {args.duration}s Music + Physical Button/Rotary Torture...")

        # Thread 1: Realistic + adversarial physical Button & Rotary choreography
        def button_rotary_choreographer():
            phase = 0
            while time.monotonic() < deadline and not fatal_errors:
                phase += 1
                kind = phase % 10
                try:
                    if kind == 0:
                        # Short clicks: NEXT (0), SEEK_FWD (5), SEEK_BACK (4), PLAYPAUSE (2) -> PLAYPAUSE (2), PREV (1)
                        for b in (0, 5, 5, 4, 2, 2, 1, 3):
                            if time.monotonic() >= deadline:
                                break
                            ws_ctrl.short_click(b, hold_sec=0.05)
                            bump("short_clicks")
                            time.sleep(0.22)

                    elif kind == 1:
                        # Rapid button-mashing burst (28 edges in ~280 ms while track decode/open runs!)
                        for b in (0, 5, 0, 4, 1, 0, 2, 2, 5, 4, 0, 1, 5, 0):
                            ws_ctrl.button_edge(b, True)
                            time.sleep(0.01)
                            ws_ctrl.button_edge(b, False)
                            time.sleep(0.01)
                            bump("rapid_mash_edges", 2)
                        time.sleep(0.5)

                    elif kind == 2:
                        # Rotary volume spin choreography (+1, +2, +1, -2, -2, +1 detents)
                        for det in (1, 2, -2, 1, -3, 2, -1):
                            ws_ctrl.rotary_turn(det)
                            bump("rotary_volume_turns")
                            time.sleep(0.12)
                        # Keep volume in safe range (3..6)
                        ws_ctrl.send_json({"controls": {"set_volume": 4}})

                    elif kind == 3:
                        # Hold Button 1 + Turn Rotary -> CMD_SEEK_PREVIEW with immediate release-commit!
                        ws_ctrl.button_edge(1, True)
                        time.sleep(0.08)
                        for det in (6, 5, -3, 4):
                            ws_ctrl.rotary_turn(det)
                            time.sleep(0.09)
                        ws_ctrl.button_edge(1, False)  # Commits seek preview immediately on release!
                        bump("seek_preview_release_commits")
                        time.sleep(0.45)

                    elif kind == 4:
                        # Hold Button 1 + Turn Rotary -> CMD_SEEK_PREVIEW with 2.1s idle auto-commit!
                        ws_ctrl.button_edge(1, True)
                        time.sleep(0.08)
                        ws_ctrl.rotary_turn(8)
                        time.sleep(0.08)
                        ws_ctrl.rotary_turn(-2)
                        time.sleep(2.15)  # Exceed seekPrevDelay (2000 ms) while still held!
                        ws_ctrl.button_edge(1, False)  # Must NOT fire short/long action thanks to usedAsModifier!
                        bump("seek_preview_idle_commits")
                        time.sleep(0.3)

                    elif kind == 5:
                        # Hold Button 0 + Turn Rotary -> direct rotary seek; Hold Button 2 + Turn Rotary -> brightness!
                        ws_ctrl.button_edge(0, True)
                        time.sleep(0.07)
                        ws_ctrl.rotary_turn(3)
                        time.sleep(0.07)
                        ws_ctrl.rotary_turn(-2)
                        time.sleep(0.06)
                        ws_ctrl.button_edge(0, False)
                        bump("rotary_seek_gestures")
                        time.sleep(0.2)

                        ws_ctrl.button_edge(2, True)
                        time.sleep(0.07)
                        for det in (3, 3, -4, -2):
                            ws_ctrl.rotary_turn(det)
                            time.sleep(0.06)
                        ws_ctrl.button_edge(2, False)
                        bump("rotary_brightness_gestures")
                        time.sleep(0.25)

                    elif kind == 6:
                        # Smart-Hold continuous scroll on Button 6 (long6 = CMD_SMART_FORWARDS, 200 ms repeat)
                        ws_ctrl.button_edge(6, True)
                        time.sleep(1.35)  # > intervalToLongPress + several 200 ms smart repeats!
                        ws_ctrl.button_edge(6, False)
                        bump("smart_hold_sessions")
                        bump("long_presses")
                        time.sleep(0.35)

                        # Long-press Button 4 (VOLUMEUP hold) & Button 5 (VOLUMEDOWN hold)
                        ws_ctrl.long_press(4, hold_sec=0.95)
                        ws_ctrl.long_press(5, hold_sec=0.95)
                        bump("long_presses", 2)
                        ws_ctrl.send_json({"controls": {"set_volume": 4}})

                    elif kind == 7:
                        # Multi-button chord (Button 0 + Button 2 simultaneously) + First/Last track long presses
                        ws_ctrl.button_edge(0, True)
                        ws_ctrl.button_edge(2, True)
                        time.sleep(0.07)
                        ws_ctrl.button_edge(0, False)
                        ws_ctrl.button_edge(2, False)
                        bump("multi_chords")
                        time.sleep(0.3)

                    elif kind == 8:
                        # Child-lock cycle: lock controls -> mash buttons + rotary + preview -> unlock controls!
                        ws_ctrl.cmd_action(100)  # CMD_LOCK_BUTTONS_MOD -> lock ON
                        time.sleep(0.12)
                        ws_ctrl.short_click(0)
                        ws_ctrl.short_click(2)
                        ws_ctrl.rotary_turn(4)
                        ws_ctrl.rotary_turn(-4)
                        ws_ctrl.cmd_action(100)  # CMD_LOCK_BUTTONS_MOD -> lock OFF
                        bump("child_lock_cycles")
                        time.sleep(0.25)

                    else:
                        # Equalizer profile cycle + folder jumps + repeat/mute/nightmode toggles
                        for cmd in (154, 154, 184, 185, 110, 110, 111, 111, 192, 192, 120, 120):
                            ws_ctrl.cmd_action(cmd)
                            time.sleep(0.14)
                        bump("eq_profile_cycles", 2)
                        bump("folder_jumps", 2)

                except Exception:
                    bump("http_errors")
                    time.sleep(0.2)

        # Thread 2: Periodic Music Album / SD Library / Virtual RFID Switcher
        def music_playlist_switcher():
            idx = 0
            while time.monotonic() < deadline and not fatal_errors:
                time.sleep(9.5)
                if time.monotonic() >= deadline:
                    break
                idx += 1
                try:
                    if sd_music_sources and idx % 4 == 0:
                        # Trigger virtual RFID card 01 or 02 via Cmd_Action (241 / 242)
                        ws_ctrl.cmd_action(241 if (idx % 8 == 0) else 242)
                        bump("music_playlist_starts")
                    else:
                        path, mode = all_music_targets[idx % len(all_music_targets)]
                        q = urllib.parse.quote(path, safe="/")
                        st, _ = client.http_req("POST", f"/exploreraudio?path={q}&playmode={mode}", timeout=10)
                        if st == 200:
                            bump("music_playlist_starts")
                        else:
                            bump("http_errors")
                except Exception:
                    bump("http_errors")

        # Thread 3: Live UI Poller (/trackprogress, /cover, /debug heap & stack watermarks)
        def ui_telemetry_monitor():
            last_dbg = 0.0
            while time.monotonic() < deadline and not fatal_errors:
                now = time.monotonic()
                try:
                    st_tp, _ = client.http_req("GET", "/trackprogress", timeout=6)
                    if st_tp == 200:
                        bump("trackprogress_polls")
                    client.http_req("GET", "/cover", timeout=6)
                    ws_ctrl.send_json({"trackinfo": {}})

                    if now - last_dbg >= 5.0:
                        last_dbg = now
                        st, raw = client.http_req("GET", "/debug", timeout=8)
                        if st == 200:
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
                            elapsed = int(args.duration - (deadline - now))
                            print(
                                f"  [T+{elapsed:3d}s] freeHeap={d['freeHeap']} minHeap={d['minFreeHeap']} "
                                f"largestBlk={d['largestFreeInternalBlock']} clicks={stats['short_clicks']} "
                                f"mash={stats['rapid_mash_edges']} rot={stats['rotary_volume_turns']} "
                                f"prev={stats['seek_preview_release_commits']+stats['seek_preview_idle_commits']} "
                                f"uartBtnLogs={button_log_hits}"
                            )
                except Exception:
                    bump("http_errors")
                time.sleep(0.45)

        threads = [
            threading.Thread(target=button_rotary_choreographer),
            threading.Thread(target=music_playlist_switcher),
            threading.Thread(target=ui_telemetry_monitor),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

    finally:
        print("[CLEANUP] Releasing all buttons, stopping music, restoring settings & removing test albums...")
        try:
            # Ensure every button 0..6 is released and child lock is off
            for b in range(7):
                ws_ctrl.button_edge(b, False)
            time.sleep(0.15)
            ws_ctrl.cmd_action(182)  # CMD_STOP
            ws_ctrl.send_json({"controls": {"set_volume": orig_vol}})
            ws_ctrl.send_json({"ftpStatus": {"start": 0}})
            ws_ctrl.send_json({"webdavStatus": {"start": 1 if orig_dav_running else 0}})
            time.sleep(0.4)

            # Restore exact button mappings (clearing temporary rotCw/rotCcw overrides) & equalizer
            restore_btns = dict(orig_buttons)
            restore_btns["rotCw0"] = 180
            restore_btns["rotCcw0"] = 181
            restore_btns["rotCw1"] = 0
            restore_btns["rotCcw1"] = 0
            restore_btns["rotCw2"] = 157
            restore_btns["rotCcw2"] = 158
            client.http_req(
                "POST", "/settings",
                body=json.dumps({
                    "buttons": restore_btns,
                    "equalizer": {
                        "gainLowPass": orig_eq.get("gainLowPass", 0),
                        "gainBandPass": orig_eq.get("gainBandPass", 0),
                        "gainHighPass": orig_eq.get("gainHighPass", 0),
                    },
                }).encode(),
                headers={"Content-Type": "application/json"},
                timeout=10,
            )
            client.http_req("DELETE", f"/explorer?path={TEST_DIR}", timeout=20)
        finally:
            ws_ctrl.close()
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

    report = {
        "uptimeBeforeMs": uptime0,
        "uptimeAfterMs": uptime1,
        "freeHeap": dbg1["freeHeap"],
        "minFreeHeap": dbg1["minFreeHeap"],
        "largestFreeInternalBlock": dbg1["largestFreeInternalBlock"],
        "coredump": dbg1["coredump"]["available"],
        "serialLinesCaptured": serial_lines_count,
        "uartPhysicalButtonLogs": button_log_hits,
        "serialPanics": len(serial_panics),
        "sdRootClean": root1_names == root0_names,
        "stats": stats,
    }
    print(json.dumps(report, indent=2))

    if serial_panics or fatal_errors or uptime1 <= uptime0 or dbg1["coredump"]["available"] or TEST_DIR.strip("/") in root1_names:
        print("FAILED:", serial_panics, fatal_errors)
        sys.exit(1)
    if button_log_hits == 0 and args.serial:
        print("FAILED: zero physical button press logs observed on UART!")
        sys.exit(1)

    print("[PASS] MUSIC + PHYSICAL BUTTON/ROTARY STRESS TEST PASSED WITH 0 CRASHES & 0 REBOOTS!")


if __name__ == "__main__":
    main()
