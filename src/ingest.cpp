#include "sae/ingest.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "sae/json.hpp"
#include "sae/time.hpp"

namespace sae {
namespace {

using json::JsonScanner;
using json::JsonValue;
using json::Token;

// Interns a scanner string token without copying when it needs no unescaping.
Id intern_token(const JsonScanner& sc, Interner* in) {
  if (sc.raw_is_literal()) return in->intern(sc.raw());
  return in->intern(sc.string_value());
}

std::string token_str(const JsonScanner& sc) {
  return sc.raw_is_literal() ? std::string(sc.raw()) : sc.string_value();
}

// Spotify's older export writes "2019-03-04 14:22" with no seconds.
bool parse_minute_stamp(std::string_view s, std::int64_t* out) {
  if (s.size() < 16) return false;
  std::string padded(s.substr(0, 16));
  padded += ":00";
  return parse_iso8601_utc(padded, out);
}

}  // namespace

bool read_file(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// ---------------------------------------------------------------------------
// Spotify Extended Streaming History
// ---------------------------------------------------------------------------

IngestStats parse_spotify_extended(std::string_view text, std::string_view default_user,
                                   Catalog* cat, std::vector<StreamEvent>* out) {
  IngestStats st;
  JsonScanner sc(text);

  if (sc.next() != Token::kArrayBegin) return st;  // not the expected shape

  const Id fallback_user = cat->users.intern(default_user);

  while (sc.next() != Token::kArrayEnd && sc.token() != Token::kEnd) {
    if (sc.token() != Token::kObjectBegin) {
      sc.skip_value();
      continue;
    }
    ++st.records_read;

    // Per-row scratch. Held as strings only for the two fields that must be
    // combined into a composite track key; everything else interns directly.
    std::int64_t ts = -1;
    std::uint32_t ms = 0;
    std::string artist, title;
    Id album = kInvalidId;
    Id user = fallback_user;
    bool skipped = false, shuffle = false, is_episode = false;

    while (sc.next() != Token::kObjectEnd && sc.token() != Token::kEnd) {
      if (sc.token() != Token::kKey) { sc.skip_value(); continue; }
      const std::string_view k = sc.raw();
      sc.next();  // move to the value

      if (k == "ts") {
        if (sc.token() == Token::kString) parse_iso8601_utc(sc.raw(), &ts);
      } else if (k == "ms_played") {
        if (sc.token() == Token::kNumber && sc.number_value() > 0) {
          ms = static_cast<std::uint32_t>(sc.number_value());
        }
      } else if (k == "master_metadata_track_name") {
        if (sc.token() == Token::kString) title = token_str(sc);
      } else if (k == "master_metadata_album_artist_name") {
        if (sc.token() == Token::kString) artist = token_str(sc);
      } else if (k == "master_metadata_album_album_name") {
        if (sc.token() == Token::kString) album = intern_token(sc, &cat->albums);
      } else if (k == "username") {
        if (sc.token() == Token::kString) user = intern_token(sc, &cat->users);
      } else if (k == "skipped") {
        skipped = sc.token() == Token::kTrue;
      } else if (k == "shuffle") {
        shuffle = sc.token() == Token::kTrue;
      } else if (k == "spotify_episode_uri" || k == "episode_name") {
        if (sc.token() == Token::kString) is_episode = true;
      } else {
        sc.skip_value();
      }
    }

    if (is_episode && (artist.empty() || title.empty())) {
      ++st.skipped_podcast;
      continue;
    }
    if (ts < 0 || artist.empty() || title.empty()) {
      ++st.skipped_malformed;
      continue;
    }

    StreamEvent e;
    e.ts = ts;
    e.user = user;
    e.artist = cat->artists.intern(artist);
    e.track = cat->intern_track(artist, title);
    e.album = album;
    e.ms_played = ms;
    e.source = Source::kSpotifyExtendedHistory;
    e.skipped = skipped;
    e.shuffle = shuffle;
    out->push_back(e);
    ++st.events_emitted;
  }
  return st;
}

// ---------------------------------------------------------------------------
// Spotify StreamingHistory (older account-data export)
// ---------------------------------------------------------------------------

IngestStats parse_spotify_streaming_history(std::string_view text, std::string_view default_user,
                                            Catalog* cat, std::vector<StreamEvent>* out) {
  IngestStats st;
  JsonScanner sc(text);
  if (sc.next() != Token::kArrayBegin) return st;

  const Id user = cat->users.intern(default_user);

  while (sc.next() != Token::kArrayEnd && sc.token() != Token::kEnd) {
    if (sc.token() != Token::kObjectBegin) { sc.skip_value(); continue; }
    ++st.records_read;

    std::int64_t ts = -1;
    std::uint32_t ms = 0;
    std::string artist, title;

    while (sc.next() != Token::kObjectEnd && sc.token() != Token::kEnd) {
      if (sc.token() != Token::kKey) { sc.skip_value(); continue; }
      const std::string_view k = sc.raw();
      sc.next();
      if (k == "endTime") {
        if (sc.token() == Token::kString) parse_minute_stamp(sc.raw(), &ts);
      } else if (k == "artistName") {
        if (sc.token() == Token::kString) artist = token_str(sc);
      } else if (k == "trackName") {
        if (sc.token() == Token::kString) title = token_str(sc);
      } else if (k == "msPlayed") {
        if (sc.token() == Token::kNumber && sc.number_value() > 0) {
          ms = static_cast<std::uint32_t>(sc.number_value());
        }
      } else {
        sc.skip_value();
      }
    }

    if (ts < 0 || artist.empty() || title.empty()) { ++st.skipped_malformed; continue; }

    StreamEvent e;
    // `endTime` is when playback ended; shift back so the event lands in the
    // hour the listening actually happened in.
    e.ts = ts - static_cast<std::int64_t>(ms / 1000);
    e.user = user;
    e.artist = cat->artists.intern(artist);
    e.track = cat->intern_track(artist, title);
    e.ms_played = ms;
    e.source = Source::kSpotifyExtendedHistory;
    out->push_back(e);
    ++st.events_emitted;
  }
  return st;
}

// ---------------------------------------------------------------------------
// Last.fm 1K dataset (TSV)
// ---------------------------------------------------------------------------

IngestStats parse_lastfm_dataset(std::string_view text, Catalog* cat,
                                 std::vector<StreamEvent>* out) {
  IngestStats st;
  std::size_t pos = 0;

  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    ++st.records_read;

    // Split on tabs in place; the dataset has exactly six columns.
    std::string_view col[6];
    std::size_t n = 0, start = 0;
    for (std::size_t i = 0; i <= line.size() && n < 6; ++i) {
      if (i == line.size() || line[i] == '\t') {
        col[n++] = line.substr(start, i - start);
        start = i + 1;
      }
    }
    if (n < 6) { ++st.skipped_malformed; continue; }

    std::int64_t ts = -1;
    if (!parse_iso8601_utc(col[1], &ts) || col[3].empty() || col[5].empty()) {
      ++st.skipped_malformed;
      continue;
    }

    StreamEvent e;
    e.ts = ts;
    e.user = cat->users.intern(col[0]);
    e.artist = cat->artists.intern(col[3]);
    e.track = cat->intern_track(col[3], col[5]);
    e.ms_played = 0;  // the dataset records scrobbles, not durations
    e.source = Source::kLastfmDataset;
    out->push_back(e);
    ++st.events_emitted;
  }
  return st;
}

// ---------------------------------------------------------------------------
// Last.fm API (user.getRecentTracks)
// ---------------------------------------------------------------------------

IngestStats parse_lastfm_api(std::string_view text, Catalog* cat, std::vector<StreamEvent>* out) {
  IngestStats st;
  const JsonValue doc = JsonValue::parse(text);
  const JsonValue& recent = doc["recenttracks"];

  // The fetcher concatenates pages into {"recenttracks":{"track":[...]}}, and a
  // raw single-page response has the same shape, so both work unchanged.
  std::string user = std::string(recent["@attr"]["user"].as_string("lastfm_user"));
  const Id uid = cat->users.intern(user);

  for (const JsonValue* t : recent["track"].as_array_or_single()) {
    ++st.records_read;
    const JsonValue& date = (*t)["date"];
    if (date.is_null()) { ++st.skipped_now_playing; continue; }

    // `uts` is a string in Last.fm's JSON.
    const std::string_view uts = date["uts"].as_string();
    if (uts.empty()) { ++st.skipped_malformed; continue; }
    std::int64_t ts = 0;
    for (char c : uts) {
      if (c < '0' || c > '9') { ts = -1; break; }
      ts = ts * 10 + (c - '0');
    }

    const std::string_view artist = (*t)["artist"]["#text"].as_string(
        (*t)["artist"]["name"].as_string());
    const std::string_view title = (*t)["name"].as_string();
    if (ts <= 0 || artist.empty() || title.empty()) { ++st.skipped_malformed; continue; }

    StreamEvent e;
    e.ts = ts;
    e.user = uid;
    e.artist = cat->artists.intern(artist);
    e.track = cat->intern_track(artist, title);
    const std::string_view album = (*t)["album"]["#text"].as_string();
    if (!album.empty()) e.album = cat->albums.intern(album);
    e.ms_played = 0;
    e.source = Source::kLastfmApi;
    out->push_back(e);
    ++st.events_emitted;
  }
  return st;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

Format detect_format(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
  if (i >= text.size()) return Format::kAuto;

  if (text[i] == '{') return Format::kLastfmApi;
  if (text[i] == '[') {
    // Both Spotify exports are arrays; the field names tell them apart.
    const std::string_view head = text.substr(i, 4096);
    if (head.find("\"master_metadata_track_name\"") != std::string_view::npos ||
        head.find("\"ms_played\"") != std::string_view::npos) {
      return Format::kSpotifyExtended;
    }
    if (head.find("\"endTime\"") != std::string_view::npos ||
        head.find("\"msPlayed\"") != std::string_view::npos) {
      return Format::kSpotifyHistory;
    }
    return Format::kSpotifyExtended;
  }
  // Tab-separated with an ISO timestamp in column 2 is the Last.fm dataset.
  if (text.find('\t') != std::string_view::npos) return Format::kLastfmDataset;
  return Format::kAuto;
}

const char* format_name(Format f) {
  switch (f) {
    case Format::kSpotifyExtended: return "spotify-extended";
    case Format::kSpotifyHistory: return "spotify-history";
    case Format::kLastfmDataset: return "lastfm-dataset";
    case Format::kLastfmApi: return "lastfm-api";
    case Format::kAuto: return "auto";
  }
  return "unknown";
}

IngestStats ingest_path(const std::string& path, Format fmt, std::string_view default_user,
                        Catalog* cat, std::vector<StreamEvent>* out, std::string* error) {
  IngestStats st;
  std::string text;
  if (!read_file(path, &text)) {
    if (error) *error = "cannot open " + path;
    return st;
  }
  if (fmt == Format::kAuto) fmt = detect_format(text);

  try {
    switch (fmt) {
      case Format::kSpotifyExtended:
        st = parse_spotify_extended(text, default_user, cat, out);
        break;
      case Format::kSpotifyHistory:
        st = parse_spotify_streaming_history(text, default_user, cat, out);
        break;
      case Format::kLastfmDataset:
        st = parse_lastfm_dataset(text, cat, out);
        break;
      case Format::kLastfmApi:
        st = parse_lastfm_api(text, cat, out);
        break;
      case Format::kAuto:
        if (error) *error = "unrecognised format: " + path;
        break;
    }
  } catch (const json::ParseError& e) {
    if (error) *error = std::string(path) + ": " + e.what();
  }
  return st;
}

}  // namespace sae
