#!/usr/bin/env python3
"""Pull real Spotify listening data into the Extended Streaming History shape.

Spotify exposes listening history two ways, and they answer different questions:

  --mode recent   GET /v1/me/player/recently-played. Live, but Spotify keeps
                  only the last 50 plays, so this is for keeping a dataset
                  current rather than building one.
  --mode export   Your "Extended Streaming History" download from
                  https://www.spotify.com/account/privacy — years of plays with
                  real durations and skip flags. Point this at the unzipped
                  folder and it merges the per-year files into one input.

Auth for --mode recent is the Authorization Code flow with PKCE, so no client
secret is needed and nothing but a short-lived token touches disk.

Usage:
  # one-off: register an app at https://developer.spotify.com/dashboard
  # and add http://127.0.0.1:8888/callback as a redirect URI
  export SPOTIFY_CLIENT_ID=...
  python3 tools/fetch_spotify.py --mode recent --out data/spotify_recent.json

  python3 tools/fetch_spotify.py --mode export \
      --export-dir ~/Downloads/my_spotify_data --out data/spotify_history.json

Standard library only.
"""

from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import http.server
import json
import os
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API_ROOT = "https://api.spotify.com/v1"
REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-recently-played"


# ---------------------------------------------------------------------------
# PKCE auth
# ---------------------------------------------------------------------------

class _CallbackHandler(http.server.BaseHTTPRequestHandler):
    code = None
    error = None
    state = None

    def do_GET(self):  # noqa: N802 (name fixed by the base class)
        params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        _CallbackHandler.code = params.get("code", [None])[0]
        _CallbackHandler.error = params.get("error", [None])[0]
        _CallbackHandler.state = params.get("state", [None])[0]
        body = (b"<h2>Authorized. You can close this tab.</h2>"
                if _CallbackHandler.code else b"<h2>Authorization failed.</h2>")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass  # keep the console clean


def authorize(client_id: str) -> str:
    """Runs the PKCE flow and returns an access token."""
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(64)).decode().rstrip("=")
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
    state = secrets.token_urlsafe(16)

    query = urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "state": state,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    })
    # A one-shot loopback server: handle_request serves exactly the redirect and
    # returns, so there is nothing left listening once the token is in hand.
    server = http.server.HTTPServer(("127.0.0.1", 8888), _CallbackHandler)
    worker = threading.Thread(target=server.handle_request, daemon=True)
    worker.start()

    url = f"{AUTH_URL}?{query}"
    print("opening your browser to authorize; if it does not open, visit:\n  " + url)
    webbrowser.open(url)
    worker.join(timeout=300)
    server.server_close()

    if worker.is_alive():
        raise SystemExit("timed out waiting for the Spotify redirect")
    if _CallbackHandler.error or not _CallbackHandler.code:
        raise SystemExit(f"authorization failed: {_CallbackHandler.error or 'no code returned'}")
    # Verifying `state` is what stops a forged redirect from injecting someone
    # else's authorization code into this exchange.
    if not secrets.compare_digest(_CallbackHandler.state or "", state):
        raise SystemExit("authorization failed: state mismatch on the redirect")

    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": _CallbackHandler.code,
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "code_verifier": verifier,
    }).encode()
    req = urllib.request.Request(TOKEN_URL, data=data,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode())["access_token"]


# ---------------------------------------------------------------------------
# Mode: recent
# ---------------------------------------------------------------------------

def fetch_recent(token: str, limit: int = 50) -> list:
    url = f"{API_ROOT}/me/player/recently-played?limit={min(limit, 50)}"
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        raise SystemExit(f"spotify returned HTTP {exc.code}: {exc.read()[:200]!r}")

    rows = []
    for item in payload.get("items", []):
        track = item.get("track") or {}
        artists = track.get("artists") or [{}]
        album = track.get("album") or {}
        # recently-played reports when a play *ended* and does not say how much
        # was heard, so duration_ms is the best available stand-in.
        rows.append({
            "ts": item.get("played_at", "").replace(".000Z", "Z"),
            "ms_played": track.get("duration_ms", 0),
            "master_metadata_track_name": track.get("name"),
            "master_metadata_album_artist_name": artists[0].get("name"),
            "master_metadata_album_album_name": album.get("name"),
            "spotify_track_uri": track.get("uri"),
            "reason_end": "trackdone",
            "shuffle": False,
            "skipped": False,
        })
    return rows


# ---------------------------------------------------------------------------
# Mode: export
# ---------------------------------------------------------------------------

def merge_export(export_dir: str) -> list:
    """Merges the JSON files out of an unzipped Spotify data download."""
    patterns = [
        "**/Streaming_History_Audio*.json",   # extended history
        "**/StreamingHistory*.json",          # older account-data export
        "**/endsong*.json",                   # what the extended export used to be called
    ]
    paths = []
    for pattern in patterns:
        paths.extend(sorted(glob.glob(os.path.join(export_dir, pattern), recursive=True)))
    if not paths:
        raise SystemExit(
            f"no streaming-history JSON found under {export_dir}\n"
            "request 'Extended streaming history' at https://www.spotify.com/account/privacy, "
            "then unzip the download and point --export-dir at it")

    rows = []
    for path in paths:
        with open(path, "r", encoding="utf-8") as fh:
            chunk = json.load(fh)
        rows.extend(chunk)
        print(f"  {os.path.basename(path):<50} {len(chunk):>8} rows")
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Spotify listening data for the engine")
    ap.add_argument("--mode", choices=["recent", "export"], default="recent")
    ap.add_argument("--out", default="data/spotify_recent.json")
    ap.add_argument("--client-id", default=os.environ.get("SPOTIFY_CLIENT_ID", ""))
    ap.add_argument("--export-dir", default="", help="unzipped Spotify data download")
    ap.add_argument("--merge", action="store_true",
                    help="append to --out instead of replacing it, de-duplicating by timestamp")
    args = ap.parse_args()

    if args.mode == "export":
        if not args.export_dir:
            print("--mode export needs --export-dir", file=sys.stderr)
            return 2
        rows = merge_export(args.export_dir)
    else:
        if not args.client_id:
            print("no client id: set SPOTIFY_CLIENT_ID or pass --client-id\n"
                  "create an app at https://developer.spotify.com/dashboard and add\n"
                  f"  {REDIRECT_URI}\nas a redirect URI", file=sys.stderr)
            return 2
        rows = fetch_recent(authorize(args.client_id))

    if args.merge and os.path.exists(args.out):
        with open(args.out, "r", encoding="utf-8") as fh:
            rows = json.load(fh) + rows
        seen, deduped = set(), []
        for row in rows:
            key = (row.get("ts"), row.get("master_metadata_track_name"))
            if key in seen:
                continue
            seen.add(key)
            deduped.append(row)
        rows = deduped

    rows.sort(key=lambda r: r.get("ts") or r.get("endTime") or "")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, ensure_ascii=False)

    print(f"wrote {len(rows)} plays to {args.out}")
    print(f"next: ./build/sae {args.out} --out dashboard/report.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
