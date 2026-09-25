// The analytics pipeline.
//
// Structure: one pass over the event vector fills a set of hash-map
// accumulators (`Aggregates`); a second, cheap pass turns each accumulator into
// a ranking via bounded heaps (`Report`). Separating accumulation from ranking
// is what makes the pipeline modular — a new metric is a new accumulator plus a
// new heap, and neither touches ingest or output.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sae/event.hpp"
#include "sae/topk.hpp"

namespace sae {

struct EngineConfig {
  std::size_t top_k = 25;
  // Gap that ends a listening session, in seconds. 30 min is the usual
  // convention for scrobble data and matches Spotify's own session heuristic.
  std::int64_t session_gap_sec = 1800;
  // A play shorter than this counts as a skip when the source does not say.
  std::uint32_t skip_threshold_ms = 30000;
  // Length of the two windows compared for trend momentum, in days.
  int trend_window_days = 28;
  // Ranking weight: 0 = pure play count, 1 = pure listening time. Sources
  // without durations always fall back to play counts.
  double time_weight = 0.5;
};

// Per-entity counters. One of these per distinct artist / track / album.
struct EntityStat {
  std::uint64_t plays = 0;
  std::uint64_t ms = 0;
  std::uint64_t skips = 0;
  std::int64_t first_ts = 0;
  std::int64_t last_ts = 0;
  std::uint32_t distinct_days = 0;   // filled during finalisation
  std::uint64_t recent_plays = 0;    // plays in the trailing window
  std::uint64_t prior_plays = 0;     // plays in the window before that
};

struct SessionSummary {
  std::uint64_t count = 0;
  std::uint64_t total_events = 0;
  std::uint64_t total_ms = 0;
  std::int64_t longest_sec = 0;
  double mean_events = 0;
};

// Everything the accumulation pass produces.
struct Aggregates {
  std::unordered_map<Id, EntityStat> artists;
  std::unordered_map<Id, EntityStat> tracks;
  std::unordered_map<Id, EntityStat> albums;

  // Distinct-day sets, kept separately so EntityStat stays small and POD.
  std::unordered_map<Id, std::vector<std::int64_t>> artist_days;

  std::array<std::uint64_t, 24> hour_plays{};
  std::array<std::uint64_t, 7> weekday_plays{};
  std::array<std::array<std::uint64_t, 24>, 7> weekday_hour{};

  std::unordered_map<std::int64_t, std::uint64_t> daily_plays;   // day index -> plays
  std::unordered_map<Id, std::uint64_t> per_user_plays;
  std::unordered_map<int, std::uint64_t> per_source_plays;

  std::uint64_t total_events = 0;
  std::uint64_t total_ms = 0;
  std::uint64_t total_skips = 0;
  std::uint64_t events_with_duration = 0;
  std::int64_t min_ts = 0;
  std::int64_t max_ts = 0;

  SessionSummary sessions;
};

// A ranked row, resolved to display strings.
struct RankedRow {
  std::string name;
  std::string secondary;  // artist name for tracks, empty otherwise
  std::uint64_t plays = 0;
  std::uint64_t ms = 0;
  std::uint64_t skips = 0;
  std::uint32_t distinct_days = 0;
  double score = 0;
  double momentum = 0;    // recent vs prior window; meaningful only when
  bool has_momentum = false;  // ... this is set (the artist-keyed stages)
  std::int64_t first_ts = 0;
  std::int64_t last_ts = 0;
};

struct Report {
  std::vector<RankedRow> top_artists;
  std::vector<RankedRow> top_tracks;
  std::vector<RankedRow> top_albums;
  std::vector<RankedRow> rising;    // strongest positive momentum
  std::vector<RankedRow> falling;   // strongest negative momentum
  std::vector<RankedRow> loyalty;   // artists ranked by breadth of days listened

  std::array<std::uint64_t, 24> hour_plays{};
  std::array<std::uint64_t, 7> weekday_plays{};
  std::array<std::array<std::uint64_t, 24>, 7> weekday_hour{};

  // Chronological daily play counts, for the dashboard's trend line.
  std::vector<std::pair<std::int64_t, std::uint64_t>> daily;

  std::uint64_t total_events = 0;
  std::uint64_t total_ms = 0;
  std::uint64_t distinct_artists = 0;
  std::uint64_t distinct_tracks = 0;
  std::uint64_t distinct_albums = 0;
  std::uint64_t distinct_users = 0;
  double skip_rate = 0;
  double artist_concentration = 0;  // share of plays held by the top 10 artists
  std::int64_t first_ts = 0;
  std::int64_t last_ts = 0;
  SessionSummary sessions;
  std::vector<std::pair<std::string, std::uint64_t>> per_source;

  // Wall-clock timings of each stage, in milliseconds.
  double ingest_ms = 0;
  double aggregate_ms = 0;
  double rank_ms = 0;
};

// Pass 1: fold every event into the accumulators. O(N) with O(distinct) memory.
// `events` is sorted by timestamp in place, which the session pass requires.
void accumulate(std::vector<StreamEvent>& events, const EngineConfig& cfg, Aggregates* agg);

// Pass 2: turn accumulators into rankings. O(D log K) per dimension.
Report rank(const Aggregates& agg, const Catalog& cat, const EngineConfig& cfg);

// Convenience wrapper for the two passes.
Report run_pipeline(std::vector<StreamEvent>& events, const Catalog& cat, const EngineConfig& cfg);

// Serialises a report to JSON for the dashboard.
std::string report_to_json(const Report& rep, const IngestStats& ingest, const EngineConfig& cfg);

}  // namespace sae
