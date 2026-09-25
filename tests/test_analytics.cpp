#include <string>
#include <vector>

#include "sae/analytics.hpp"
#include "sae/json.hpp"
#include "sae/time.hpp"
#include "json_shape.hpp"
#include "test_framework.hpp"

using namespace sae;

namespace {

struct Fixture {
  Catalog cat;
  std::vector<StreamEvent> events;

  void add(const char* user, const char* artist, const char* title, const char* iso,
           std::uint32_t ms = 200000) {
    StreamEvent e;
    if (!parse_iso8601_utc(iso, &e.ts)) e.ts = 0;
    e.user = cat.users.intern(user);
    e.artist = cat.artists.intern(artist);
    e.track = cat.intern_track(artist, title);
    e.ms_played = ms;
    events.push_back(e);
  }
};

}  // namespace

TEST(analytics, counts_plays_time_and_distincts) {
  Fixture f;
  f.add("me", "Bon Iver", "Holocene", "2024-01-01T10:00:00Z", 300000);
  f.add("me", "Bon Iver", "Holocene", "2024-01-01T10:10:00Z", 300000);
  f.add("me", "Radiohead", "Nude", "2024-01-01T10:20:00Z", 200000);

  EngineConfig cfg;
  const Report rep = run_pipeline(f.events, f.cat, cfg);

  CHECK_EQ(rep.total_events, static_cast<std::uint64_t>(3));
  CHECK_EQ(rep.distinct_artists, static_cast<std::uint64_t>(2));
  CHECK_EQ(rep.distinct_tracks, static_cast<std::uint64_t>(2));
  CHECK_EQ(rep.total_ms, static_cast<std::uint64_t>(800000));
  CHECK_EQ(rep.top_artists[0].name, std::string("Bon Iver"));
  CHECK_EQ(rep.top_artists[0].plays, static_cast<std::uint64_t>(2));
  CHECK_EQ(rep.top_tracks[0].name, std::string("Holocene"));
  CHECK_EQ(rep.top_tracks[0].secondary, std::string("Bon Iver"));
}

TEST(analytics, same_title_by_different_artists_stays_separate) {
  Fixture f;
  f.add("me", "Artist A", "Intro", "2024-01-01T10:00:00Z");
  f.add("me", "Artist B", "Intro", "2024-01-01T11:00:00Z");
  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  CHECK_EQ(rep.distinct_tracks, static_cast<std::uint64_t>(2));
}

TEST(analytics, time_weight_changes_the_ranking) {
  Fixture f;
  // Many short plays vs few long ones: the weight decides which wins.
  for (int i = 0; i < 10; ++i) {
    f.add("me", "Frequent", "Short Song", "2024-01-01T10:00:00Z", 60000);
  }
  for (int i = 0; i < 2; ++i) {
    f.add("me", "Lengthy", "Long Piece", "2024-01-02T10:00:00Z", 2400000);
  }

  EngineConfig by_plays;
  by_plays.time_weight = 0.0;
  std::vector<StreamEvent> a = f.events;
  CHECK_EQ(run_pipeline(a, f.cat, by_plays).top_artists[0].name, std::string("Frequent"));

  EngineConfig by_time;
  by_time.time_weight = 1.0;
  std::vector<StreamEvent> b = f.events;
  CHECK_EQ(run_pipeline(b, f.cat, by_time).top_artists[0].name, std::string("Lengthy"));
}

TEST(analytics, sessions_split_on_the_configured_gap) {
  Fixture f;
  f.add("me", "A", "1", "2024-01-01T10:00:00Z");
  f.add("me", "A", "2", "2024-01-01T10:05:00Z");   // same session
  f.add("me", "A", "3", "2024-01-01T12:00:00Z");   // 115 min later: new session
  f.add("me", "A", "4", "2024-01-01T12:04:00Z");

  EngineConfig cfg;
  cfg.session_gap_sec = 1800;
  const Report rep = run_pipeline(f.events, f.cat, cfg);
  CHECK_EQ(rep.sessions.count, static_cast<std::uint64_t>(2));
  CHECK_NEAR(rep.sessions.mean_events, 2.0, 1e-9);
}

TEST(analytics, sessions_are_tracked_per_user) {
  Fixture f;
  // Two users interleaved: their plays must not merge into one session.
  f.add("alice", "A", "1", "2024-01-01T10:00:00Z");
  f.add("bob", "B", "1", "2024-01-01T10:01:00Z");
  f.add("alice", "A", "2", "2024-01-01T10:02:00Z");
  f.add("bob", "B", "2", "2024-01-01T10:03:00Z");

  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  CHECK_EQ(rep.sessions.count, static_cast<std::uint64_t>(2));
  CHECK_EQ(rep.distinct_users, static_cast<std::uint64_t>(2));
}

TEST(analytics, skip_rate_uses_the_threshold_when_the_source_is_silent) {
  Fixture f;
  f.add("me", "A", "full", "2024-01-01T10:00:00Z", 240000);
  f.add("me", "A", "abandoned", "2024-01-01T10:05:00Z", 5000);
  EngineConfig cfg;
  cfg.skip_threshold_ms = 30000;
  const Report rep = run_pipeline(f.events, f.cat, cfg);
  CHECK_NEAR(rep.skip_rate, 0.5, 1e-9);
}

TEST(analytics, durationless_sources_never_look_like_skips) {
  // A Last.fm scrobble carries ms_played = 0. Treating that as a 0 ms play
  // would report a 100% skip rate, which is the bug this guards.
  Fixture f;
  f.add("me", "A", "1", "2024-01-01T10:00:00Z", 0);
  f.add("me", "A", "2", "2024-01-01T10:05:00Z", 0);
  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  CHECK_NEAR(rep.skip_rate, 0.0, 1e-9);
  CHECK_EQ(rep.top_artists[0].plays, static_cast<std::uint64_t>(2));
}

TEST(analytics, hour_and_weekday_histograms) {
  Fixture f;
  f.add("me", "A", "1", "2024-01-01T03:00:00Z");  // a Monday
  f.add("me", "A", "2", "2024-01-01T03:30:00Z");
  f.add("me", "A", "3", "2024-01-06T22:00:00Z");  // a Saturday

  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  CHECK_EQ(rep.hour_plays[3], static_cast<std::uint64_t>(2));
  CHECK_EQ(rep.hour_plays[22], static_cast<std::uint64_t>(1));
  CHECK_EQ(rep.weekday_plays[1], static_cast<std::uint64_t>(2));  // Monday
  CHECK_EQ(rep.weekday_plays[6], static_cast<std::uint64_t>(1));  // Saturday
  CHECK_EQ(rep.weekday_hour[1][3], static_cast<std::uint64_t>(2));
}

TEST(analytics, momentum_separates_rising_from_falling) {
  Fixture f;
  // "Rising" is played only recently, "Falling" only in the older window.
  for (int i = 0; i < 10; ++i) f.add("me", "Falling", "old", "2024-01-10T10:00:00Z");
  for (int i = 0; i < 10; ++i) f.add("me", "Rising", "new", "2024-03-01T10:00:00Z");

  EngineConfig cfg;
  cfg.trend_window_days = 28;
  const Report rep = run_pipeline(f.events, f.cat, cfg);

  CHECK(!rep.rising.empty());
  CHECK_EQ(rep.rising[0].name, std::string("Rising"));
  CHECK_NEAR(rep.rising[0].momentum, 1.0, 1e-9);
  CHECK(!rep.falling.empty());
  CHECK_EQ(rep.falling[0].name, std::string("Falling"));
  CHECK_NEAR(rep.falling[0].momentum, -1.0, 1e-9);
}

TEST(analytics, trends_ignore_artists_below_the_noise_floor) {
  Fixture f;
  f.add("me", "OneHit", "x", "2024-03-01T10:00:00Z");  // a single recent play
  for (int i = 0; i < 10; ++i) f.add("me", "Real", "y", "2024-03-02T10:00:00Z");
  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  for (const RankedRow& r : rep.rising) CHECK(r.name != std::string("OneHit"));
}

TEST(analytics, loyalty_rewards_breadth_over_bingeing) {
  Fixture f;
  // Binger: 8 plays in one day. Steady: 4 plays across 4 separate days.
  for (int i = 0; i < 8; ++i) f.add("me", "Binger", "b", "2024-02-01T10:00:00Z");
  f.add("me", "Steady", "s", "2024-02-01T10:00:00Z");
  f.add("me", "Steady", "s", "2024-02-02T10:00:00Z");
  f.add("me", "Steady", "s", "2024-02-03T10:00:00Z");
  f.add("me", "Steady", "s", "2024-02-04T10:00:00Z");

  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  CHECK_EQ(rep.top_artists[0].name, std::string("Binger"));   // volume
  CHECK_EQ(rep.loyalty[0].name, std::string("Steady"));       // breadth
  CHECK_EQ(rep.loyalty[0].distinct_days, 4u);
}

TEST(analytics, top_k_is_respected) {
  Fixture f;
  for (int i = 0; i < 50; ++i) {
    const std::string name = "Artist " + std::to_string(i);
    for (int p = 0; p <= i; ++p) f.add("me", name.c_str(), "t", "2024-01-01T10:00:00Z");
  }
  EngineConfig cfg;
  cfg.top_k = 5;
  const Report rep = run_pipeline(f.events, f.cat, cfg);
  CHECK_EQ(rep.top_artists.size(), static_cast<std::size_t>(5));
  CHECK_EQ(rep.top_artists[0].name, std::string("Artist 49"));
  CHECK_EQ(rep.top_artists[4].name, std::string("Artist 45"));
}

TEST(analytics, empty_input_is_not_a_crash) {
  Catalog cat;
  std::vector<StreamEvent> none;
  const Report rep = run_pipeline(none, cat, EngineConfig{});
  CHECK_EQ(rep.total_events, static_cast<std::uint64_t>(0));
  CHECK(rep.top_artists.empty());
  CHECK_NEAR(rep.skip_rate, 0.0, 1e-9);
}

TEST(analytics, report_json_is_valid_and_complete) {
  Fixture f;
  f.add("me", "Bon Iver", "Holocene", "2024-01-01T10:00:00Z", 300000);
  f.add("me", "Bon Iver", "Skinny Love", "2024-01-01T10:06:00Z", 250000);
  f.add("me", "Radiohead", "Nude", "2024-01-02T22:00:00Z", 260000);

  IngestStats st;
  st.records_read = 3;
  st.events_emitted = 3;
  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  const std::string doc = report_to_json(rep, st, EngineConfig{});

  // The dashboard feeds this to a strict JSON.parse, so the document has to be
  // well-formed and not merely readable by our own lenient scanner.
  const std::string defect = ::testing::separator_defect(doc);
  if (!defect.empty()) {
    ::testing::report_failure(__FILE__, __LINE__, "report JSON is well-formed", defect);
  }

  const json::JsonValue v = json::JsonValue::parse(doc);
  CHECK_NEAR(v["summary"]["total_events"].as_number(), 3.0, 1e-9);
  CHECK_NEAR(v["summary"]["distinct_artists"].as_number(), 2.0, 1e-9);
  CHECK_EQ(v["top_artists"].size(), static_cast<std::size_t>(2));
  CHECK_EQ(std::string(v["top_artists"].at(0)["name"].as_string()), std::string("Bon Iver"));
  CHECK_EQ(std::string(v["top_tracks"].at(0)["artist"].as_string()), std::string("Bon Iver"));
  CHECK_EQ(v["patterns"]["by_hour"].size(), static_cast<std::size_t>(24));
  CHECK_EQ(v["patterns"]["weekday_hour"].size(), static_cast<std::size_t>(7));
  CHECK_EQ(v["patterns"]["weekday_hour"].at(0).size(), static_cast<std::size_t>(24));
  CHECK_EQ(v["daily"].size(), static_cast<std::size_t>(2));
  CHECK_EQ(std::string(v["daily"].at(0)["date"].as_string()), std::string("2024-01-01"));
  CHECK_NEAR(v["ingest"]["records_read"].as_number(), 3.0, 1e-9);
}

TEST(analytics, json_survives_names_needing_escapes) {
  Fixture f;
  f.add("me", "Say \"Yes\"\tNow", "line\nbreak", "2024-01-01T10:00:00Z");
  const Report rep = run_pipeline(f.events, f.cat, EngineConfig{});
  const std::string doc = report_to_json(rep, IngestStats{}, EngineConfig{});
  const json::JsonValue v = json::JsonValue::parse(doc);
  CHECK_EQ(std::string(v["top_artists"].at(0)["name"].as_string()),
           std::string("Say \"Yes\"\tNow"));
}
