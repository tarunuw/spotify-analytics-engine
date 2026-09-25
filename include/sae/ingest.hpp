// Ingest: three real-world source formats, one normalized event stream.
//
// Adding a fourth source means adding one function here and one line in
// `ingest_path` — nothing downstream of `StreamEvent` changes.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sae/event.hpp"

namespace sae {

// Reads a whole file into memory. Returns false if it cannot be opened.
bool read_file(const std::string& path, std::string* out);

// Spotify "Extended Streaming History": a JSON array of flat objects with
// `ts`, `ms_played`, `master_metadata_track_name`,
// `master_metadata_album_artist_name`, `master_metadata_album_album_name`,
// `skipped`, `shuffle`. Podcast rows carry episode fields instead and are
// counted as skipped_podcast. Parsed with the streaming scanner.
IngestStats parse_spotify_extended(std::string_view text, std::string_view default_user,
                                   Catalog* cat, std::vector<StreamEvent>* out);

// Spotify's older "StreamingHistory*.json" account-data export: objects with
// `endTime` ("YYYY-MM-DD HH:MM"), `artistName`, `trackName`, `msPlayed`.
IngestStats parse_spotify_streaming_history(std::string_view text, std::string_view default_user,
                                            Catalog* cat, std::vector<StreamEvent>* out);

// Last.fm 1K dataset (userid-timestamp-artid-artname-traid-traname.tsv):
// user \t ISO-8601 UTC \t mbid \t artist \t mbid \t track. The dataset carries
// no play duration, so ms_played is left 0 and duration-weighted metrics fall
// back to play counts.
IngestStats parse_lastfm_dataset(std::string_view text, Catalog* cat,
                                 std::vector<StreamEvent>* out);

// Last.fm API `user.getRecentTracks` JSON page(s). Rows with
// `@attr.nowplaying` have no scrobble timestamp and are skipped.
IngestStats parse_lastfm_api(std::string_view text, Catalog* cat, std::vector<StreamEvent>* out);

enum class Format { kAuto, kSpotifyExtended, kSpotifyHistory, kLastfmDataset, kLastfmApi };

// Sniffs the format from the file's first bytes when `fmt` is kAuto.
Format detect_format(std::string_view text);
const char* format_name(Format f);

// Ingests one file, dispatching on format.
IngestStats ingest_path(const std::string& path, Format fmt, std::string_view default_user,
                        Catalog* cat, std::vector<StreamEvent>* out, std::string* error);

}  // namespace sae
