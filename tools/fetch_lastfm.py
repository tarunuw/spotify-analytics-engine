#!/usr/bin/env python3
"""Pull a real Last.fm scrobble history into a file the engine can ingest.

Last.fm's user.getRecentTracks endpoint serves a user's complete scrobble
history, which is what makes it the practical source for a large real dataset:
an active account accumulates tens of thousands of plays and the API will hand
over all of them, 200 at a time.

The pages are concatenated into a single document with the same shape as one
page, so the C++ `lastfm-api` parser reads either without a special case.

Usage:
  export LASTFM_API_KEY=...            # https://www.last.fm/api/account/create
  python3 tools/fetch_lastfm.py --user rj --out data/lastfm_recent.json

  # Only what is new since the last run:
  python3 tools/fetch_lastfm.py --user rj --out data/lastfm_recent.json --resume

Standard library only, so it runs anywhere the engine builds.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API_ROOT = "https://ws.audioscrobbler.com/2.0/"
PAGE_LIMIT = 200  # the endpoint's maximum


def api_get(params: dict, retries: int = 4) -> dict:
    """One API call, with backoff on the failures this endpoint actually has."""
    url = API_ROOT + "?" + urllib.parse.urlencode(params)
    delay = 1.0
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=30) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            # 429 and 5xx are transient; 4xx otherwise means the request is wrong.
            if exc.code != 429 and exc.code < 500:
                raise SystemExit(f"last.fm returned HTTP {exc.code}: {exc.read()[:200]!r}")
            if attempt == retries - 1:
                raise SystemExit(f"last.fm kept returning HTTP {exc.code}")
        except (urllib.error.URLError, TimeoutError) as exc:
            if attempt == retries - 1:
                raise SystemExit(f"network error talking to last.fm: {exc}")
        else:
            # Last.fm reports its own errors inside a 200 response.
            if "error" in payload:
                raise SystemExit(f"last.fm error {payload['error']}: {payload.get('message')}")
            return payload
        time.sleep(delay)
        delay *= 2
    raise SystemExit("unreachable")


def existing_latest_uts(path: str) -> int:
    """Newest scrobble already saved, so --resume can ask for only what is new."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError):
        return 0
    newest = 0
    for track in doc.get("recenttracks", {}).get("track", []):
        uts = track.get("date", {}).get("uts")
        if uts:
            newest = max(newest, int(uts))
    return newest


def fetch(user: str, api_key: str, since: int, max_pages: int) -> list:
    base = {
        "method": "user.getrecenttracks",
        "user": user,
        "api_key": api_key,
        "format": "json",
        "limit": PAGE_LIMIT,
        "extended": 0,
    }
    if since:
        base["from"] = since + 1

    first = api_get(dict(base, page=1))
    attr = first.get("recenttracks", {}).get("@attr", {})
    total_pages = int(attr.get("totalPages", 1) or 1)
    total = int(attr.get("total", 0) or 0)
    pages_to_read = min(total_pages, max_pages) if max_pages else total_pages
    print(f"{user}: {total} scrobbles across {total_pages} pages; reading {pages_to_read}")

    tracks = list(first.get("recenttracks", {}).get("track", []))
    if isinstance(tracks, dict):  # single-result pages come back unwrapped
        tracks = [tracks]

    for page in range(2, pages_to_read + 1):
        payload = api_get(dict(base, page=page))
        page_tracks = payload.get("recenttracks", {}).get("track", [])
        if isinstance(page_tracks, dict):
            page_tracks = [page_tracks]
        tracks.extend(page_tracks)
        if page % 10 == 0 or page == pages_to_read:
            print(f"  page {page}/{pages_to_read} — {len(tracks)} tracks", flush=True)
        # The published rate limit is ~5 requests/second per key; stay well under.
        time.sleep(0.25)
    return tracks


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Last.fm scrobbles for the analytics engine")
    ap.add_argument("--user", required=True, help="Last.fm username")
    ap.add_argument("--out", default="data/lastfm_recent.json")
    ap.add_argument("--api-key", default=os.environ.get("LASTFM_API_KEY", ""))
    ap.add_argument("--max-pages", type=int, default=0, help="0 = every page")
    ap.add_argument("--resume", action="store_true",
                    help="only fetch scrobbles newer than those already in --out")
    args = ap.parse_args()

    if not args.api_key:
        print("no API key: set LASTFM_API_KEY or pass --api-key\n"
              "get one at https://www.last.fm/api/account/create", file=sys.stderr)
        return 2

    since = existing_latest_uts(args.out) if args.resume else 0
    if since:
        print(f"resuming from {time.strftime('%Y-%m-%d %H:%M', time.gmtime(since))} UTC")

    new_tracks = fetch(args.user, args.api_key, since, args.max_pages)

    if args.resume and since:
        with open(args.out, "r", encoding="utf-8") as fh:
            old = json.load(fh)["recenttracks"]["track"]
        new_tracks = new_tracks + old

    # "Now playing" rows have no date and are dropped by the C++ parser anyway;
    # removing them here keeps the saved file a clean historical record.
    scrobbles = [t for t in new_tracks if t.get("date")]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    doc = {"recenttracks": {"track": scrobbles,
                            "@attr": {"user": args.user, "total": str(len(scrobbles))}}}
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False)

    print(f"wrote {len(scrobbles)} scrobbles to {args.out}")
    print(f"next: ./build/sae {args.out} --out dashboard/report.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
