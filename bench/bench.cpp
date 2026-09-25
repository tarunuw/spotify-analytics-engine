// Benchmark harness.
//
// Two questions it answers, both of which the design claims and neither of
// which should be taken on faith:
//   1. Does bounded top-K selection actually beat sorting every candidate?
//   2. How many events per second does the whole pipeline sustain?
//
// Run: ./sae_bench [event-count] [distinct-artists]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "sae/analytics.hpp"
#include "sae/event.hpp"
#include "sae/topk.hpp"

using namespace sae;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A Zipfian-ish generator: real listening is heavily skewed, and a uniform
// distribution would make the hash maps and heaps look easier than they are.
std::size_t zipf_pick(std::mt19937& rng, std::size_t n) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const double x = u(rng);
  const auto idx = static_cast<std::size_t>(static_cast<double>(n) * x * x * x);
  return std::min(idx, n - 1);
}

struct Dataset {
  Catalog cat;
  std::vector<StreamEvent> events;
};

Dataset synth(std::size_t n_events, std::size_t n_artists, unsigned seed = 7) {
  Dataset d;
  std::mt19937 rng(seed);
  std::vector<Id> artists, tracks;
  artists.reserve(n_artists);
  for (std::size_t i = 0; i < n_artists; ++i) {
    const std::string name = "Artist " + std::to_string(i);
    artists.push_back(d.cat.artists.intern(name));
    for (int t = 0; t < 8; ++t) {
      tracks.push_back(d.cat.intern_track(name, "Track " + std::to_string(t)));
    }
  }

  const Id user = d.cat.users.intern("bench");
  std::int64_t ts = 1600000000;
  std::uniform_int_distribution<int> gap(30, 400);
  std::uniform_int_distribution<int> dur(20000, 320000);

  d.events.reserve(n_events);
  for (std::size_t i = 0; i < n_events; ++i) {
    const std::size_t a = zipf_pick(rng, n_artists);
    StreamEvent e;
    e.ts = ts;
    ts += gap(rng);
    e.user = user;
    e.artist = artists[a];
    e.track = tracks[a * 8 + (i % 8)];
    e.ms_played = static_cast<std::uint32_t>(dur(rng));
    d.events.push_back(e);
  }
  return d;
}

// The reference implementation top-K replaces: materialise every candidate,
// sort them all, take a prefix.
std::vector<Ranked> full_sort_topk(const std::unordered_map<Id, EntityStat>& src, std::size_t k) {
  std::vector<Ranked> all;
  all.reserve(src.size());
  for (const auto& [id, s] : src) {
    Ranked r;
    r.key = id;
    r.score = static_cast<double>(s.plays);
    r.tie = id;
    all.push_back(r);
  }
  std::sort(all.begin(), all.end(), RankBetter{});
  if (all.size() > k) all.resize(k);
  return all;
}

std::vector<Ranked> heap_topk(const std::unordered_map<Id, EntityStat>& src, std::size_t k) {
  TopK heap(k);
  for (const auto& [id, s] : src) {
    Ranked r;
    r.key = id;
    r.score = static_cast<double>(s.plays);
    r.tie = id;
    heap.offer(r);
  }
  return heap.take();
}

void bench_topk(const Aggregates& agg) {
  std::printf("\nTop-K selection over %zu distinct artists\n", agg.artists.size());
  std::printf("  %6s %12s %12s %9s %8s\n", "K", "full sort", "bounded heap", "speedup", "match");

  for (std::size_t k : {10u, 25u, 100u, 1000u}) {
    // Repeat enough that a single run's noise cannot dominate.
    const int reps = 50;
    std::vector<Ranked> a, b;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) a = full_sort_topk(agg.artists, k);
    const double sort_ms = ms_since(t0) / reps;

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) b = heap_topk(agg.artists, k);
    const double heap_ms = ms_since(t0) / reps;

    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i].key == b[i].key;

    std::printf("  %6zu %10.3f ms %10.3f ms %8.2fx %8s\n", k, sort_ms, heap_ms,
                heap_ms > 0 ? sort_ms / heap_ms : 0.0, same ? "yes" : "NO");
  }
}

void bench_pipeline(std::size_t n_events, std::size_t n_artists) {
  Dataset d = synth(n_events, n_artists);
  EngineConfig cfg;

  auto t0 = std::chrono::steady_clock::now();
  Aggregates agg;
  accumulate(d.events, cfg, &agg);
  const double acc_ms = ms_since(t0);

  t0 = std::chrono::steady_clock::now();
  const Report rep = rank(agg, d.cat, cfg);
  const double rank_ms = ms_since(t0);

  std::printf("\n%zu events · %zu artists · %zu tracks\n", n_events, agg.artists.size(),
              agg.tracks.size());
  std::printf("  accumulate  %8.2f ms  (%.2f M events/s)\n", acc_ms,
              acc_ms > 0 ? static_cast<double>(n_events) / acc_ms / 1000.0 : 0.0);
  std::printf("  rank        %8.2f ms\n", rank_ms);
  std::printf("  total       %8.2f ms  -> top artist: %s (%llu plays)\n", acc_ms + rank_ms,
              rep.top_artists.empty() ? "-" : rep.top_artists[0].name.c_str(),
              rep.top_artists.empty() ? 0ull
                                      : static_cast<unsigned long long>(rep.top_artists[0].plays));

  bench_topk(agg);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n_events = argc > 1 ? std::stoul(argv[1]) : 50000;
  const std::size_t n_artists = argc > 2 ? std::stoul(argv[2]) : 4000;

  std::printf("sae benchmark — g++/clang -O2, single thread\n");
  bench_pipeline(n_events, n_artists);
  if (argc == 1) {
    // Scaling check: the pipeline should stay roughly linear in event count.
    bench_pipeline(500000, 20000);
  }
  return 0;
}
