#include <string>
#include <vector>

#include "sae/ingest.hpp"
#include "sae/time.hpp"
#include "test_framework.hpp"

using namespace sae;

namespace {

// A two-row slice in Spotify's real Extended Streaming History shape, including
// the fields the parser is expected to ignore and a podcast row it must drop.
const char* kSpotifyExtended = R"([
  {
    "ts": "2024-01-15T08:30:00Z",
    "username": "tarun",
    "platform": "android",
    "ms_played": 214000,
    "conn_country": "US",
    "ip_addr_decrypted": "0.0.0.0",
    "user_agent_decrypted": "unknown",
    "master_metadata_track_name": "Holocene",
    "master_metadata_album_artist_name": "Bon Iver",
    "master_metadata_album_album_name": "Bon Iver, Bon Iver",
    "spotify_track_uri": "spotify:track:1",
    "episode_name": null,
    "episode_show_name": null,
    "spotify_episode_uri": null,
    "reason_start": "trackdone",
    "reason_end": "trackdone",
    "shuffle": true,
    "skipped": false,
    "offline": false,
    "offline_timestamp": 0,
    "incognito_mode": false
  },
  {
    "ts": "2024-01-15T08:34:00Z",
    "username": "tarun",
    "ms_played": 4000,
    "master_metadata_track_name": "Skinny Love",
    "master_metadata_album_artist_name": "Bon Iver",
    "master_metadata_album_album_name": "For Emma, Forever Ago",
    "shuffle": false,
    "skipped": true
  },
  {
    "ts": "2024-01-15T09:00:00Z",
    "ms_played": 1800000,
    "master_metadata_track_name": null,
    "master_metadata_album_artist_name": null,
    "episode_name": "Some Podcast",
    "spotify_episode_uri": "spotify:episode:9"
  }
])";

const char* kSpotifyHistory = R"([
  {"endTime": "2019-03-04 14:22", "artistName": "Radiohead",
   "trackName": "Weird Fishes", "msPlayed": 313000}
])";

// Real column order of userid-timestamp-artid-artname-traid-traname.tsv.
const char* kLastfmTsv =
    "user_000001\t2009-05-04T23:08:57Z\tmbid-1\tDeerhunter\tmbid-2\tNothing Ever Happened\n"
    "user_000001\t2009-05-04T23:12:12Z\tmbid-1\tDeerhunter\tmbid-3\tAgoraphobia\n"
    "user_000002\t2009-05-05T01:00:00Z\t\tThe National\t\tFake Empire\n"
    "malformed row without tabs\n";

const char* kLastfmApi = R"({"recenttracks":{
  "track":[
    {"artist":{"mbid":"","#text":"Phoebe Bridgers"},
     "album":{"#text":"Punisher"},
     "name":"Kyoto",
     "date":{"uts":"1700000000","#text":"14 Nov 2023, 22:13"}},
    {"artist":{"#text":"Phoebe Bridgers"},"name":"Now Playing Track",
     "@attr":{"nowplaying":"true"}}
  ],
  "@attr":{"user":"tarun","total":"2"}}})";

}  // namespace

TEST(ingest, spotify_extended_reads_fields_and_drops_podcasts) {
  Catalog cat;
  std::vector<StreamEvent> ev;
  const IngestStats st = parse_spotify_extended(kSpotifyExtended, "fallback", &cat, &ev);

  CHECK_EQ(st.records_read, static_cast<std::size_t>(3));
  CHECK_EQ(st.events_emitted, static_cast<std::size_t>(2));
  CHECK_EQ(st.skipped_podcast, static_cast<std::size_t>(1));
  CHECK_EQ(ev.size(), static_cast<std::size_t>(2));

  CHECK_EQ(cat.artists.str(ev[0].artist), std::string("Bon Iver"));
  CHECK_EQ(cat.users.str(ev[0].user), std::string("tarun"));  // username beats the fallback
  CHECK_EQ(cat.albums.str(ev[0].album), std::string("Bon Iver, Bon Iver"));
  CHECK_EQ(ev[0].ms_played, 214000u);
  CHECK(ev[0].shuffle);
  CHECK(!ev[0].skipped);
  CHECK(ev[1].skipped);

  const auto parts = Catalog::split_track(cat.tracks.str(ev[0].track));
  CHECK_EQ(std::string(parts.first), std::string("Bon Iver"));
  CHECK_EQ(std::string(parts.second), std::string("Holocene"));
}

TEST(ingest, spotify_history_backdates_from_end_time) {
  Catalog cat;
  std::vector<StreamEvent> ev;
  const IngestStats st = parse_spotify_streaming_history(kSpotifyHistory, "me", &cat, &ev);
  CHECK_EQ(st.events_emitted, static_cast<std::size_t>(1));

  // endTime is when playback stopped, so the event is shifted back by its
  // duration to land in the hour the listening happened.
  std::int64_t end = 0;
  parse_iso8601_utc("2019-03-04T14:22:00Z", &end);
  CHECK_EQ(ev[0].ts, end - 313);
  CHECK_EQ(cat.artists.str(ev[0].artist), std::string("Radiohead"));
  CHECK_EQ(cat.users.str(ev[0].user), std::string("me"));  // export carries no username
}

TEST(ingest, lastfm_dataset_parses_tsv_and_rejects_bad_rows) {
  Catalog cat;
  std::vector<StreamEvent> ev;
  const IngestStats st = parse_lastfm_dataset(kLastfmTsv, &cat, &ev);

  CHECK_EQ(st.records_read, static_cast<std::size_t>(4));
  CHECK_EQ(st.events_emitted, static_cast<std::size_t>(3));
  CHECK_EQ(st.skipped_malformed, static_cast<std::size_t>(1));
  CHECK_EQ(cat.users.size(), static_cast<std::size_t>(2));
  CHECK_EQ(cat.artists.str(ev[2].artist), std::string("The National"));
  CHECK_EQ(ev[0].ms_played, 0u);  // the dataset has no durations
}

TEST(ingest, lastfm_api_skips_now_playing) {
  Catalog cat;
  std::vector<StreamEvent> ev;
  const IngestStats st = parse_lastfm_api(kLastfmApi, &cat, &ev);

  CHECK_EQ(st.records_read, static_cast<std::size_t>(2));
  CHECK_EQ(st.events_emitted, static_cast<std::size_t>(1));
  CHECK_EQ(st.skipped_now_playing, static_cast<std::size_t>(1));
  CHECK_EQ(ev[0].ts, static_cast<std::int64_t>(1700000000));
  CHECK_EQ(cat.users.str(ev[0].user), std::string("tarun"));
  CHECK_EQ(cat.albums.str(ev[0].album), std::string("Punisher"));
}

TEST(ingest, format_detection) {
  CHECK(detect_format(kSpotifyExtended) == Format::kSpotifyExtended);
  CHECK(detect_format(kSpotifyHistory) == Format::kSpotifyHistory);
  CHECK(detect_format(kLastfmTsv) == Format::kLastfmDataset);
  CHECK(detect_format(kLastfmApi) == Format::kLastfmApi);
  CHECK(detect_format("   \n") == Format::kAuto);
}

TEST(ingest, sources_share_one_catalog) {
  // The point of normalising: a Spotify play and a Last.fm scrobble of the same
  // artist must land on the same interned id so they aggregate together.
  Catalog cat;
  std::vector<StreamEvent> ev;
  parse_spotify_extended(
      R"([{"ts":"2024-01-01T00:00:00Z","ms_played":1000,
           "master_metadata_track_name":"Kyoto",
           "master_metadata_album_artist_name":"Phoebe Bridgers"}])",
      "me", &cat, &ev);
  parse_lastfm_api(kLastfmApi, &cat, &ev);

  CHECK_EQ(ev.size(), static_cast<std::size_t>(2));
  CHECK_EQ(ev[0].artist, ev[1].artist);
  CHECK_EQ(ev[0].track, ev[1].track);
  CHECK_EQ(cat.artists.size(), static_cast<std::size_t>(1));
}

TEST(ingest, interner_ids_are_stable_across_growth) {
  // The interner keys its table by views into its own storage; growth must not
  // invalidate them.
  Interner in;
  std::vector<Id> ids;
  for (int i = 0; i < 5000; ++i) ids.push_back(in.intern("artist-" + std::to_string(i)));
  for (int i = 0; i < 5000; ++i) {
    CHECK_EQ(in.intern("artist-" + std::to_string(i)), ids[static_cast<std::size_t>(i)]);
    CHECK_EQ(in.str(ids[static_cast<std::size_t>(i)]), "artist-" + std::to_string(i));
  }
  CHECK_EQ(in.size(), static_cast<std::size_t>(5000));
  CHECK_EQ(in.lookup("never-seen"), kInvalidId);
}
