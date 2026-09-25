// The normalized record every source is flattened into.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sae/intern.hpp"

namespace sae {

enum class Source : std::uint8_t {
  kSpotifyExtendedHistory,
  kLastfmDataset,
  kLastfmApi,
};

inline const char* source_name(Source s) {
  switch (s) {
    case Source::kSpotifyExtendedHistory: return "spotify_extended_history";
    case Source::kLastfmDataset: return "lastfm_1k_dataset";
    case Source::kLastfmApi: return "lastfm_api";
  }
  return "unknown";
}

// 32 bytes, trivially copyable, no owned allocations: a 50K-event run fits in
// ~1.6 MB and a 10M-event run still streams comfortably. All text lives in the
// Catalog's interners and is referenced by id.
struct StreamEvent {
  std::int64_t ts = 0;        // epoch seconds, UTC
  Id user = kInvalidId;
  Id artist = kInvalidId;
  Id track = kInvalidId;
  Id album = kInvalidId;
  std::uint32_t ms_played = 0;
  Source source = Source::kLastfmApi;
  bool skipped = false;       // true only when the source reports it
  bool shuffle = false;
};

// The shared symbol tables. Passing one Catalog through every parser is what
// lets events from Spotify and Last.fm be ranked in the same aggregation.
struct Catalog {
  Interner users;
  Interner artists;
  Interner tracks;   // keyed "Artist\x1FTrack" so same-titled songs stay distinct
  Interner albums;

  Id intern_track(std::string_view artist, std::string_view title) {
    std::string key;
    key.reserve(artist.size() + title.size() + 1);
    key.append(artist).push_back('\x1F');
    key.append(title);
    return tracks.intern(key);
  }

  // Splits a composite track key back into its display halves.
  static std::pair<std::string_view, std::string_view> split_track(const std::string& key) {
    const auto pos = key.find('\x1F');
    if (pos == std::string::npos) return {std::string_view{}, std::string_view(key)};
    return {std::string_view(key).substr(0, pos), std::string_view(key).substr(pos + 1)};
  }
};

// What a parser produced, including what it had to throw away. Reporting
// rejects rather than silently dropping them is what makes ingest auditable.
struct IngestStats {
  std::size_t records_read = 0;
  std::size_t events_emitted = 0;
  std::size_t skipped_podcast = 0;      // Spotify episodes have no track metadata
  std::size_t skipped_malformed = 0;
  std::size_t skipped_now_playing = 0;  // Last.fm API rows with no scrobble time

  void merge(const IngestStats& o) {
    records_read += o.records_read;
    events_emitted += o.events_emitted;
    skipped_podcast += o.skipped_podcast;
    skipped_malformed += o.skipped_malformed;
    skipped_now_playing += o.skipped_now_playing;
  }
};

}  // namespace sae
