#include "sae/analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include "sae/json.hpp"
#include "sae/time.hpp"

namespace sae {
namespace {

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

void touch(EntityStat* s, const StreamEvent& e, bool counts_as_skip) {
  if (s->plays == 0) {
    s->first_ts = e.ts;
    s->last_ts = e.ts;
  } else {
    if (e.ts < s->first_ts) s->first_ts = e.ts;
    if (e.ts > s->last_ts) s->last_ts = e.ts;
  }
  ++s->plays;
  s->ms += e.ms_played;
  if (counts_as_skip) ++s->skips;
}

// Blends play count and listening time into one comparable score. Time is
// log-damped so a handful of very long tracks cannot outrank a genuinely
// frequent artist, and the two terms are normalised against the dataset's own
// maxima so the weight means the same thing on any input.
double score_of(const EntityStat& s, double max_plays, double max_ms, double w) {
  const double p = max_plays > 0 ? static_cast<double>(s.plays) / max_plays : 0.0;
  if (max_ms <= 0) return p;  // duration-free source: play count is the signal
  const double t = std::log1p(static_cast<double>(s.ms)) / std::log1p(max_ms);
  return (1.0 - w) * p + w * t;
}

// Momentum in [-1, 1]: +1 means every play is in the recent window, -1 means
// every play predates it. Symmetric, bounded, and defined when either side is
// zero, which a plain ratio is not.
double momentum_of(const EntityStat& s) {
  const double r = static_cast<double>(s.recent_plays);
  const double p = static_cast<double>(s.prior_plays);
  const double denom = r + p;
  return denom > 0 ? (r - p) / denom : 0.0;
}

// `with_momentum` is false for the track and album stages: trend windows are
// accumulated per artist only, so a zero there would read as "flat" when the
// truth is "not measured".
RankedRow make_row(const Ranked& r, const EntityStat& s, std::string name, std::string secondary,
                   bool with_momentum = false) {
  RankedRow row;
  row.name = std::move(name);
  row.secondary = std::move(secondary);
  row.plays = s.plays;
  row.ms = s.ms;
  row.skips = s.skips;
  row.distinct_days = s.distinct_days;
  row.score = r.score;
  row.momentum = with_momentum ? momentum_of(s) : 0.0;
  row.has_momentum = with_momentum;
  row.first_ts = s.first_ts;
  row.last_ts = s.last_ts;
  return row;
}

// Runs one ranking stage: scan the map, offer every entity to a bounded heap,
// drain it. This is the whole of the "modular ranking pipeline" — each call is
// an independent stage over its own map and scoring function.
template <typename ScoreFn, typename NameFn>
std::vector<RankedRow> rank_stage(const std::unordered_map<Id, EntityStat>& src, std::size_t k,
                                  ScoreFn score, NameFn name, bool with_momentum = false) {
  TopK heap(k);
  for (const auto& [id, stat] : src) {
    Ranked r;
    r.key = id;
    r.score = score(stat);
    r.plays = stat.plays;
    r.ms = stat.ms;
    r.tie = id;  // deterministic ordering for equal scores
    heap.offer(r);
  }
  std::vector<RankedRow> out;
  for (const Ranked& r : heap.take()) {
    const EntityStat& s = src.at(r.key);
    auto [primary, secondary] = name(r.key);
    out.push_back(make_row(r, s, std::move(primary), std::move(secondary), with_momentum));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pass 1: accumulation
// ---------------------------------------------------------------------------

void accumulate(std::vector<StreamEvent>& events, const EngineConfig& cfg, Aggregates* agg) {
  if (events.empty()) return;

  // Session detection and window bounds both need chronological order.
  std::sort(events.begin(), events.end(),
            [](const StreamEvent& a, const StreamEvent& b) { return a.ts < b.ts; });

  agg->min_ts = events.front().ts;
  agg->max_ts = events.back().ts;

  const std::int64_t window = static_cast<std::int64_t>(cfg.trend_window_days) * 86400;
  const std::int64_t recent_cut = agg->max_ts - window;
  const std::int64_t prior_cut = recent_cut - window;

  // Reserving up front keeps the maps out of rehash churn on large inputs.
  agg->artists.reserve(events.size() / 8 + 16);
  agg->tracks.reserve(events.size() / 4 + 16);

  // Session state, per user: a gap longer than session_gap_sec ends a session.
  struct SessionState {
    std::int64_t last_ts = 0;
    std::int64_t start_ts = 0;
    std::uint64_t events = 0;
    bool open = false;
  };
  std::unordered_map<Id, SessionState> sessions;

  auto close_session = [&](SessionState& s) {
    if (!s.open) return;
    ++agg->sessions.count;
    agg->sessions.total_events += s.events;
    const std::int64_t len = s.last_ts - s.start_ts;
    if (len > agg->sessions.longest_sec) agg->sessions.longest_sec = len;
    s.open = false;
  };

  for (const StreamEvent& e : events) {
    // A source that reports skips is trusted; otherwise a very short play is
    // treated as one, and a source with no durations at all never is.
    const bool counts_as_skip =
        e.skipped || (e.ms_played > 0 && e.ms_played < cfg.skip_threshold_ms);

    ++agg->total_events;
    agg->total_ms += e.ms_played;
    if (e.ms_played > 0) ++agg->events_with_duration;
    if (counts_as_skip) ++agg->total_skips;

    EntityStat& a = agg->artists[e.artist];
    touch(&a, e, counts_as_skip);
    touch(&agg->tracks[e.track], e, counts_as_skip);
    if (e.album != kInvalidId) touch(&agg->albums[e.album], e, counts_as_skip);

    const std::int64_t day = day_index(e.ts);
    // Chronological order means the day list is already sorted; only append
    // when the day changes, which makes it a distinct-day set for free.
    auto& days = agg->artist_days[e.artist];
    if (days.empty() || days.back() != day) days.push_back(day);

    if (e.ts > recent_cut) {
      ++a.recent_plays;
    } else if (e.ts > prior_cut) {
      ++a.prior_plays;
    }

    const unsigned h = hour_utc(e.ts);
    const unsigned wd = weekday_utc(e.ts);
    ++agg->hour_plays[h];
    ++agg->weekday_plays[wd];
    ++agg->weekday_hour[wd][h];
    ++agg->daily_plays[day];
    ++agg->per_user_plays[e.user];
    ++agg->per_source_plays[static_cast<int>(e.source)];

    SessionState& s = sessions[e.user];
    if (!s.open || e.ts - s.last_ts > cfg.session_gap_sec) {
      close_session(s);
      s.open = true;
      s.start_ts = e.ts;
      s.events = 0;
    }
    s.last_ts = e.ts;
    ++s.events;
    agg->sessions.total_ms += e.ms_played;
  }

  for (auto& [user, s] : sessions) {
    (void)user;
    close_session(s);
  }
  if (agg->sessions.count > 0) {
    agg->sessions.mean_events =
        static_cast<double>(agg->sessions.total_events) / static_cast<double>(agg->sessions.count);
  }

  // Fold the day lists back into the artist stats.
  for (auto& [id, days] : agg->artist_days) {
    auto it = agg->artists.find(id);
    if (it != agg->artists.end()) {
      it->second.distinct_days = static_cast<std::uint32_t>(days.size());
    }
  }
}

// ---------------------------------------------------------------------------
// Pass 2: ranking
// ---------------------------------------------------------------------------

Report rank(const Aggregates& agg, const Catalog& cat, const EngineConfig& cfg) {
  Report rep;
  rep.total_events = agg.total_events;
  rep.total_ms = agg.total_ms;
  rep.distinct_artists = agg.artists.size();
  rep.distinct_tracks = agg.tracks.size();
  rep.distinct_albums = agg.albums.size();
  rep.distinct_users = agg.per_user_plays.size();
  rep.first_ts = agg.min_ts;
  rep.last_ts = agg.max_ts;
  rep.hour_plays = agg.hour_plays;
  rep.weekday_plays = agg.weekday_plays;
  rep.weekday_hour = agg.weekday_hour;
  rep.sessions = agg.sessions;
  rep.skip_rate = agg.total_events > 0
                      ? static_cast<double>(agg.total_skips) / static_cast<double>(agg.total_events)
                      : 0.0;

  for (const auto& [src, plays] : agg.per_source_plays) {
    rep.per_source.emplace_back(source_name(static_cast<Source>(src)), plays);
  }
  std::sort(rep.per_source.begin(), rep.per_source.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Per-dimension maxima, so scores are comparable across datasets of any size.
  auto maxima = [](const std::unordered_map<Id, EntityStat>& m) {
    double mp = 0, mm = 0;
    for (const auto& [id, s] : m) {
      (void)id;
      mp = std::max(mp, static_cast<double>(s.plays));
      mm = std::max(mm, static_cast<double>(s.ms));
    }
    return std::pair<double, double>{mp, mm};
  };

  // Plain locals, not structured bindings: C++17 forbids capturing the latter
  // in the scoring lambdas below.
  const std::pair<double, double> amax = maxima(agg.artists);
  const std::pair<double, double> tmax = maxima(agg.tracks);
  const std::pair<double, double> bmax = maxima(agg.albums);
  const double amax_p = amax.first, amax_m = amax.second;
  const double tmax_p = tmax.first, tmax_m = tmax.second;
  const double bmax_p = bmax.first, bmax_m = bmax.second;
  const double w = cfg.time_weight;

  rep.top_artists = rank_stage(
      agg.artists, cfg.top_k,
      [&](const EntityStat& s) { return score_of(s, amax_p, amax_m, w); },
      [&](Id id) { return std::pair<std::string, std::string>{cat.artists.str(id), std::string{}}; },
      /*with_momentum=*/true);

  rep.top_tracks = rank_stage(
      agg.tracks, cfg.top_k,
      [&](const EntityStat& s) { return score_of(s, tmax_p, tmax_m, w); },
      [&](Id id) {
        auto [artist, title] = Catalog::split_track(cat.tracks.str(id));
        return std::pair<std::string, std::string>{std::string(title), std::string(artist)};
      });

  rep.top_albums = rank_stage(
      agg.albums, cfg.top_k,
      [&](const EntityStat& s) { return score_of(s, bmax_p, bmax_m, w); },
      [&](Id id) { return std::pair<std::string, std::string>{cat.albums.str(id), std::string{}}; });

  // Loyalty: breadth of days listened, not raw volume. An artist played once a
  // day for a year outranks one binged over a weekend.
  rep.loyalty = rank_stage(
      agg.artists, cfg.top_k,
      [&](const EntityStat& s) { return static_cast<double>(s.distinct_days); },
      [&](Id id) { return std::pair<std::string, std::string>{cat.artists.str(id), std::string{}}; },
      /*with_momentum=*/true);

  // Trends: only artists with enough plays across the two windows to be more
  // than noise are eligible, so a single new play cannot top the chart.
  const std::uint64_t min_trend_plays = 5;
  TopK up(cfg.top_k), down(cfg.top_k);
  for (const auto& [id, s] : agg.artists) {
    if (s.recent_plays + s.prior_plays < min_trend_plays) continue;
    const double m = momentum_of(s);
    Ranked r;
    r.key = id;
    r.plays = s.plays;
    r.ms = s.ms;
    r.tie = id;
    // Weight momentum by volume so bigger movers rank above marginal ones.
    const double weight = std::log1p(static_cast<double>(s.recent_plays + s.prior_plays));
    r.score = m * weight;
    up.offer(r);
    r.score = -m * weight;
    down.offer(r);
  }
  for (const Ranked& r : up.take()) {
    const EntityStat& s = agg.artists.at(r.key);
    if (momentum_of(s) <= 0) continue;
    rep.rising.push_back(make_row(r, s, cat.artists.str(r.key), std::string{}, true));
  }
  for (const Ranked& r : down.take()) {
    const EntityStat& s = agg.artists.at(r.key);
    if (momentum_of(s) >= 0) continue;
    rep.falling.push_back(make_row(r, s, cat.artists.str(r.key), std::string{}, true));
  }

  // Concentration: what share of all plays the top 10 artists hold. A quick
  // read on whether listening is broad or narrow.
  std::uint64_t top10 = 0;
  for (std::size_t i = 0; i < rep.top_artists.size() && i < 10; ++i) {
    top10 += rep.top_artists[i].plays;
  }
  rep.artist_concentration =
      agg.total_events > 0 ? static_cast<double>(top10) / static_cast<double>(agg.total_events) : 0.0;

  rep.daily.assign(agg.daily_plays.begin(), agg.daily_plays.end());
  std::sort(rep.daily.begin(), rep.daily.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  return rep;
}

Report run_pipeline(std::vector<StreamEvent>& events, const Catalog& cat, const EngineConfig& cfg) {
  Aggregates agg;
  const double t0 = now_ms();
  accumulate(events, cfg, &agg);
  const double t1 = now_ms();
  Report rep = rank(agg, cat, cfg);
  const double t2 = now_ms();
  rep.aggregate_ms = t1 - t0;
  rep.rank_ms = t2 - t1;
  return rep;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

namespace {

std::string iso_date(std::int64_t epoch_sec) {
  const CivilDate d = civil_from_days(day_index(epoch_sec));
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(d.year), d.month, d.day);
  return buf;
}

void write_rows(json::JsonWriter& w, std::string_view key, const std::vector<RankedRow>& rows) {
  w.key(key).begin_array();
  for (const RankedRow& r : rows) {
    w.begin_object();
    w.field("name", r.name);
    if (!r.secondary.empty()) w.field("artist", r.secondary);
    w.field("plays", r.plays);
    w.field("ms", r.ms);
    w.field("minutes", static_cast<double>(r.ms) / 60000.0, 1);
    w.field("skips", r.skips);
    if (r.distinct_days > 0) w.field("distinct_days", static_cast<std::int64_t>(r.distinct_days));
    w.field("score", r.score, 6);
    if (r.has_momentum) w.field("momentum", r.momentum, 4);
    w.field("first_seen", iso_date(r.first_ts));
    w.field("last_seen", iso_date(r.last_ts));
    w.end_object();
  }
  w.end_array();
}

}  // namespace

std::string report_to_json(const Report& rep, const IngestStats& ingest, const EngineConfig& cfg) {
  std::string out;
  out.reserve(1 << 16);
  json::JsonWriter w(&out);

  w.begin_object();

  w.key("meta").begin_object();
  w.field("generator", "sae — Spotify/Last.fm Analytics Engine");
  w.field("schema_version", 1);
  w.field("top_k", static_cast<std::int64_t>(cfg.top_k));
  w.field("time_weight", cfg.time_weight, 2);
  w.field("session_gap_minutes", static_cast<std::int64_t>(cfg.session_gap_sec / 60));
  w.field("trend_window_days", cfg.trend_window_days);
  w.key("timings_ms").begin_object();
  w.field("ingest", rep.ingest_ms, 2);
  w.field("aggregate", rep.aggregate_ms, 2);
  w.field("rank", rep.rank_ms, 2);
  w.end_object();
  w.end_object();

  w.key("ingest").begin_object();
  w.field("records_read", ingest.records_read);
  w.field("events_emitted", ingest.events_emitted);
  w.field("skipped_podcast", ingest.skipped_podcast);
  w.field("skipped_malformed", ingest.skipped_malformed);
  w.field("skipped_now_playing", ingest.skipped_now_playing);
  w.key("by_source").begin_array();
  for (const auto& [name, plays] : rep.per_source) {
    w.begin_object().field("source", name).field("plays", plays).end_object();
  }
  w.end_array();
  w.end_object();

  w.key("summary").begin_object();
  w.field("total_events", rep.total_events);
  w.field("total_minutes", static_cast<double>(rep.total_ms) / 60000.0, 1);
  w.field("total_hours", static_cast<double>(rep.total_ms) / 3600000.0, 1);
  w.field("distinct_artists", rep.distinct_artists);
  w.field("distinct_tracks", rep.distinct_tracks);
  w.field("distinct_albums", rep.distinct_albums);
  w.field("distinct_users", rep.distinct_users);
  w.field("skip_rate", rep.skip_rate, 4);
  w.field("top10_artist_share", rep.artist_concentration, 4);
  w.field("first_seen", iso_date(rep.first_ts));
  w.field("last_seen", iso_date(rep.last_ts));
  w.key("sessions").begin_object();
  w.field("count", rep.sessions.count);
  w.field("mean_events", rep.sessions.mean_events, 2);
  w.field("longest_minutes", static_cast<double>(rep.sessions.longest_sec) / 60.0, 1);
  w.end_object();
  w.end_object();

  write_rows(w, "top_artists", rep.top_artists);
  write_rows(w, "top_tracks", rep.top_tracks);
  write_rows(w, "top_albums", rep.top_albums);
  write_rows(w, "rising", rep.rising);
  write_rows(w, "falling", rep.falling);
  write_rows(w, "loyalty", rep.loyalty);

  w.key("patterns").begin_object();
  w.key("by_hour").begin_array();
  for (std::uint64_t v : rep.hour_plays) w.value(static_cast<std::int64_t>(v));
  w.end_array();
  w.key("by_weekday").begin_array();
  for (std::uint64_t v : rep.weekday_plays) w.value(static_cast<std::int64_t>(v));
  w.end_array();
  w.key("weekday_hour").begin_array();
  for (const auto& row : rep.weekday_hour) {
    w.begin_array();
    for (std::uint64_t v : row) w.value(static_cast<std::int64_t>(v));
    w.end_array();
  }
  w.end_array();
  w.end_object();

  w.key("daily").begin_array();
  for (const auto& [day, plays] : rep.daily) {
    w.begin_object();
    w.field("date", iso_date(day * 86400));
    w.field("plays", plays);
    w.end_object();
  }
  w.end_array();

  w.end_object();
  out.push_back('\n');
  return out;
}

}  // namespace sae
