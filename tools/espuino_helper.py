#!/usr/bin/env python3
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

# Default config file path (placed in the repository root)
CONFIG_FILE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    ".espuino_config.json",
)


def load_config():
    """Loads configuration from environment variables or local json config."""
    # 1. Try environment variables first
    ip = os.environ.get("ESPUINO_IP")
    user = os.environ.get("ESPUINO_USER")
    password = os.environ.get("ESPUINO_PASS")

    # 2. Try local config file if env vars are missing
    if not (ip and user and password) and os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                ip = ip or data.get("ip")
                user = user or data.get("user")
                password = password or data.get("password")
        except Exception as e:
            print(f"Warning: Failed to read local config file: {e}", file=sys.stderr)

    # 3. Fallbacks / Defaults
    user = user or "espuino"

    # Device address and password are deliberately never hard-coded in the repository.
    if not ip or not password:
        missing = ", ".join(
            name for name, value in (("ESPUINO_IP", ip), ("ESPUINO_PASS", password)) if not value
        )
        print(
            f"Error: Missing {missing}. Set the environment variables or create .espuino_config.json",
            file=sys.stderr,
        )
        sys.exit(1)

    return ip, user, password


ESPUINO_IP = None
USER = None
PASS = None


def get_auth_headers():
    return {"X-API-Key": PASS}


def make_request(url, method="GET", data=None):
    headers = get_auth_headers()
    if data:
        data_bytes = json.dumps(data).encode('utf-8')
        headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data_bytes, headers=headers, method=method)
    else:
        req = urllib.request.Request(url, headers=headers, method=method)

    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            if response.status == 200:
                return response.read().decode('utf-8')
    except Exception as e:
        print(f"Error calling {url}: {e}", file=sys.stderr)
    return None


def list_dir(path):
    encoded_path = urllib.parse.quote(path)
    url = f"http://{ESPUINO_IP}/explorer?path={encoded_path}"
    res = make_request(url)
    if res:
        try:
            return json.loads(res)
        except Exception:
            pass
    return []


def scan_recursive(path, files_list):
    items = list_dir(path)
    for item in items:
        name = item.get("name")
        if not name or name in [".", ".."]:
            continue
        full_path = f"{path}/{name}" if path != "/" else f"/{name}"
        if item.get("dir"):
            scan_recursive(full_path, files_list)
        else:
            if name.lower().endswith((".mp3", ".m4a", ".wav", ".flac", ".ogg", ".m3u")):
                # Rule: Mask the forbidden word
                display_path = full_path.replace("/Tonies", "/SLIX2")
                files_list.append(display_path)


def list_files():
    files = []
    scan_recursive("/", files)
    for f in files:
        print(f)


def create_playlist(playlist_path, tracks):
    # Rule: Map back /SLIX2 to /Tonies for the actual API request
    real_playlist_path = playlist_path.replace("/SLIX2", "/Tonies")
    real_tracks = [t.replace("/SLIX2", "/Tonies") for t in tracks]

    url = f"http://{ESPUINO_IP}/playlist"
    payload = {"path": real_playlist_path, "tracks": real_tracks}

    res = make_request(url, method="POST", data=payload)
    if res is not None:
        print(f"Playlist '{playlist_path}' successfully created with {len(tracks)} tracks.")
        sys.exit(0)
    else:
        print("Failed to create playlist.", file=sys.stderr)
        sys.exit(1)


def main():
    global ESPUINO_IP, USER, PASS

    parser = argparse.ArgumentParser(description="ESPuino Helper CLI for Agents")
    parser.add_argument(
        "--list", action="store_true", help="List all audio files recursively (masks folder names)"
    )
    parser.add_argument(
        "--playlist", type=str, help="Path of the playlist to create (e.g. /Playlists/mix)"
    )
    parser.add_argument("--tracks", nargs="+", help="Space-separated list of track paths")

    args = parser.parse_args()

    if args.list:
        ESPUINO_IP, USER, PASS = load_config()
        list_files()
    elif args.playlist:
        if not args.tracks:
            print("Error: --tracks is required when creating a playlist.", file=sys.stderr)
            sys.exit(1)
        ESPUINO_IP, USER, PASS = load_config()
        create_playlist(args.playlist, args.tracks)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
