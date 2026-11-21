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
    """Environment > local device config > gitignored Homelab services.leoino."""
    from pathlib import Path

    local = {}
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, encoding="utf-8") as f:
            local = json.load(f)
    global_path = Path(os.environ.get(
        "HOMELAB_SECRETS_PATH", Path(__file__).resolve().parents[2] / "homelab" / "secrets.json"
    ))
    shared = {}
    if global_path.is_file():
        with global_path.open(encoding="utf-8") as f:
            shared = json.load(f).get("services", {}).get("leoino", {})
    ip = os.environ.get("ESPUINO_IP") or local.get("ip") or shared.get("url")
    user = os.environ.get("ESPUINO_USER") or local.get("user") or "espuino"
    key = (os.environ.get("ESPUINO_API_KEY") or os.environ.get("ESPUINO_PASS")
           or local.get("api_key") or local.get("password") or shared.get("api_key"))
    if not ip or not key:
        raise ValueError("Set ESPUINO_IP / ESPUINO_API_KEY or configure services.leoino in Homelab secrets.json")
    # Existing playlist/file helpers prepend http://; keep the legacy host format.
    if ip.startswith("http://"):
        ip = ip[7:]
    if "/" in ip.rstrip("/") or "@" in ip:
        raise ValueError("Device URL must be an HTTP host with optional port")
    return ip.rstrip("/"), user, key


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


def api_request(path, method="GET", data=None):
    """Authenticated access to any device API route; no redirects with credentials."""
    if not path.startswith("/") or path.startswith("//"):
        raise ValueError("API path must start with a single /")

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    headers = get_auth_headers()
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://{ESPUINO_IP}{path}", data=data,
                                 headers=headers, method=method)
    with urllib.request.build_opener(NoRedirect).open(req, timeout=30) as response:
        return response.read()


def save_private(path, data):
    from pathlib import Path
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)


def download_diagnostics(directory):
    from pathlib import Path
    import urllib.error
    target = Path(directory)
    for endpoint, filename in (("/info", "info.json"), ("/debug", "debug.json"),
                               ("/log", "log.txt"), ("/coredump", "coredump.bin")):
        try:
            data = api_request(endpoint)
        except urllib.error.HTTPError as error:
            if endpoint == "/coredump" and error.code == 404:
                print("No stored core dump.")
                continue
            raise
        save_private(target / filename, data)
        print(f"Saved {filename} ({len(data)} bytes)")


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

    parser.add_argument("--api", metavar="PATH", help="Request any API path, e.g. /debug")
    parser.add_argument("--method", default="GET", choices=["GET", "POST", "PUT", "PATCH", "DELETE"])
    parser.add_argument("--data-file", help="JSON request body file (keeps secrets out of shell arguments)")
    parser.add_argument("--output", help="Save raw response to a private file")
    parser.add_argument("--diagnostics", metavar="DIR", help="Download info, debug, log and stored core dump")
    args = parser.parse_args()

    if args.api or args.diagnostics:
        from pathlib import Path
        ESPUINO_IP, USER, PASS = load_config()
        if args.diagnostics:
            download_diagnostics(args.diagnostics)
        else:
            data = Path(args.data_file).read_bytes() if args.data_file else None
            result = api_request(args.api, args.method, data)
            if args.output:
                save_private(args.output, result)
            else:
                print(result.decode("utf-8").replace("/Tonies", "/SLIX2"))
        return

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
    try:
        main()
    except (ValueError, OSError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
