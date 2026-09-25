#!/usr/bin/env python3
"""Generate sample datasets in the exact shapes the real sources produce.

This exists so the repo is runnable and testable without anyone's credentials:
CI needs an input, and so does anyone who clones this before requesting their
Spotify export. The output is synthetic, but the *schemas* are the real ones —
the same files the parsers see in production — so exercising the pipeline here
exercises the same code paths.

For real data use tools/fetch_lastfm.py and tools/fetch_spotify.py.

Usage:
  python3 tools/make_sample.py --events 50000 --out-dir data
"""

from __future__ import annotations

import argparse
import json
import os
import random
from datetime import datetime, timedelta, timezone

ARTISTS = [
    ("Bon Iver", ["Holocene", "Skinny Love", "re: Stacks", "Perth", "715 - CRΣΣKS"],
     "Bon Iver, Bon Iver"),
    ("Radiohead", ["Weird Fishes", "Nude", "Reckoner", "Idioteque", "Let Down"], "In Rainbows"),
    ("Phoebe Bridgers", ["Kyoto", "Motion Sickness", "Punisher", "I Know the End"], "Punisher"),
    ("Tycho", ["Awake", "A Walk", "Dive", "Hours"], "Awake"),
    ("Khruangbin", ["白い花", "August 10", "Maria También", "Time (You and I)"], "Con Todo El Mundo"),
    ("Sufjan Stevens", ["Mystery of Love", "Chicago", "Should Have Known Better"], "Carrie & Lowell"),
    ("Aphex Twin", ["Xtal", "Avril 14th", "Windowlicker"], "Selected Ambient Works"),
    ("Fleet Foxes", ["White Winter Hymnal", "Helplessness Blues", "Mykonos"], "Helplessness Blues"),
    ("Nujabes", ["Feather", "Aruarian Dance", "Luv(sic) Pt. 3"], "Modal Soul"),
    ("Big Thief", ["Not", "Masterpiece", "Cattails", "Paul"], "U.F.O.F."),
    ("Caroline Polachek", ["So Hot You're Hurting", "Bunny Is a Rider"], "Pang"),
    ("Mac DeMarco", ["Chamber of Reflection", "My Kind of Woman", "Salad Days"], "Salad Days"),
    ("Frank Ocean", ["Nights", "Pink + White", "Ivy", "Self Control"], "Blonde"),
    ("Japanese Breakfast", ["Be Sweet", "Road Head", "Paprika"], "Jubilee"),
    ("Alvvays", ["Archie, Marry Me", "Dreams Tonite", "Belinda Says"], "Blue Rev"),
    ("Four Tet", ["Two Thousand and Seventeen", "Baby", "Angel Echoes"], "New Energy"),
    ("Beach House", ["Space Song", "Myth", "Silver Soul"], "Bloom"),
    ("The National", ["Fake Empire", "Bloodbuzz Ohio", "I Need My Girl"], "High Violet"),
    ("Arca", ["Nonbinary", "Time", "Prada"], "KiCk i"),
    ("Floating Points", ["Movement 1", "Last Bloom", "Silhouettes"], "Promises"),
]

# Hour-of-day weights: a plausible listener — commute peaks, an evening block,
# a quiet overnight. Makes the dashboard's heatmap show structure rather than noise.
HOUR_WEIGHTS = [2, 1, 1, 1, 1, 2, 5, 12, 18, 14, 10, 9,
                11, 10, 9, 10, 13, 20, 22, 19, 16, 12, 8, 4]


def build_plays(n_events: int, days: int, seed: int):
    rng = random.Random(seed)
    end = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
    start = end - timedelta(days=days)

    # Zipf-ish artist popularity, plus a "phase": each artist has a stretch of
    # weeks they are played most, which is what gives the trend metrics
    # something real to find.
    weights = [1.0 / (i + 1) ** 0.9 for i in range(len(ARTISTS))]
    phases = [rng.uniform(0, 1) for _ in ARTISTS]

    plays = []
    for _ in range(n_events):
        day_frac = rng.random()
        idx = rng.choices(
            range(len(ARTISTS)),
            weights=[w * (1.0 + 2.0 * max(0.0, 1.0 - abs(day_frac - p) * 6))
                     for w, p in zip(weights, phases)],
            k=1)[0]
        artist, tracks, album = ARTISTS[idx]

        when = start + timedelta(seconds=day_frac * days * 86400)
        hour = rng.choices(range(24), weights=HOUR_WEIGHTS, k=1)[0]
        when = when.replace(hour=hour, minute=rng.randrange(60), second=rng.randrange(60))

        duration = rng.randrange(150, 380) * 1000
        # ~14% of plays are abandoned early; the rest run to completion.
        skipped = rng.random() < 0.14
        ms_played = rng.randrange(2000, 28000) if skipped else duration

        plays.append({
            "when": when,
            "artist": artist,
            "track": rng.choice(tracks),
            "album": album,
            "ms_played": ms_played,
            "skipped": skipped,
            "shuffle": rng.random() < 0.55,
        })

    plays.sort(key=lambda p: p["when"])
    return plays


def write_spotify_extended(plays, path, user):
    rows = [{
        "ts": p["when"].strftime("%Y-%m-%dT%H:%M:%SZ"),
        "username": user,
        "platform": "osx",
        "ms_played": p["ms_played"],
        "conn_country": "US",
        "master_metadata_track_name": p["track"],
        "master_metadata_album_artist_name": p["artist"],
        "master_metadata_album_album_name": p["album"],
        "spotify_track_uri": "spotify:track:%032x" % (hash((p["artist"], p["track"])) & (2**128 - 1)),
        "episode_name": None,
        "episode_show_name": None,
        "spotify_episode_uri": None,
        "reason_start": "trackdone",
        "reason_end": "endplay" if p["skipped"] else "trackdone",
        "shuffle": p["shuffle"],
        "skipped": p["skipped"],
        "offline": False,
        "incognito_mode": False,
    } for p in plays]
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, ensure_ascii=False)
    return len(rows)


def write_lastfm_tsv(plays, path, user):
    with open(path, "w", encoding="utf-8") as fh:
        for p in plays:
            fh.write("\t".join([
                user,
                p["when"].strftime("%Y-%m-%dT%H:%M:%SZ"),
                "",                       # musicbrainz artist id, often blank
                p["artist"],
                "",                       # musicbrainz track id
                p["track"],
            ]) + "\n")
    return len(plays)


def write_lastfm_api(plays, path, user):
    tracks = [{
        "artist": {"mbid": "", "#text": p["artist"]},
        "album": {"mbid": "", "#text": p["album"]},
        "name": p["track"],
        "streamable": "0",
        "date": {"uts": str(int(p["when"].timestamp())),
                 "#text": p["when"].strftime("%d %b %Y, %H:%M")},
    } for p in plays]
    doc = {"recenttracks": {"track": tracks,
                            "@attr": {"user": user, "total": str(len(tracks))}}}
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False)
    return len(tracks)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate sample data in the real source formats")
    ap.add_argument("--events", type=int, default=50000)
    ap.add_argument("--days", type=int, default=730)
    ap.add_argument("--seed", type=int, default=20251201)
    ap.add_argument("--user", default="sample_user")
    ap.add_argument("--out-dir", default="data")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    plays = build_plays(args.events, args.days, args.seed)

    # Split the same listening history across the three formats, so running all
    # three files together also exercises multi-source merging.
    cut_a = int(len(plays) * 0.6)
    cut_b = int(len(plays) * 0.85)

    out = [
        ("sample_spotify_extended.json",
         write_spotify_extended(plays[:cut_a], os.path.join(args.out_dir,
                                                            "sample_spotify_extended.json"),
                                args.user)),
        ("sample_lastfm_dataset.tsv",
         write_lastfm_tsv(plays[cut_a:cut_b], os.path.join(args.out_dir,
                                                           "sample_lastfm_dataset.tsv"),
                          args.user)),
        ("sample_lastfm_api.json",
         write_lastfm_api(plays[cut_b:], os.path.join(args.out_dir, "sample_lastfm_api.json"),
                          args.user)),
    ]
    for name, count in out:
        print(f"  {name:<34} {count:>7} events")
    print(f"\n{len(plays)} events total across {args.days} days")
    print(f"next: ./build/sae {args.out_dir}/sample_* --out dashboard/report.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
