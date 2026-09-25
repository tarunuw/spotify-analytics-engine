// CLI driver: ingest one or more real exports, run the pipeline, print a
// summary and optionally emit the dashboard's report JSON.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "sae/analytics.hpp"
#include "sae/ingest.hpp"
#include "sae/time.hpp"

namespace {

using namespace sae;

void usage() {
  std::cout <<
      R"(sae — Spotify/Last.fm Analytics Engine

Usage:
  sae [options] <input-file>...

Inputs may be any mix of:
  Spotify "Extended Streaming History"  Streaming_History_Audio_*.json
  Spotify account-data export           StreamingHistory*.json
  Last.fm 1K dataset                    userid-timestamp-artid-artname-traid-traname.tsv
  Last.fm API recent tracks             fetched by tools/fetch_lastfm.py

The format of each file is detected from its contents unless --format is given.

Options:
  -o, --out <file>        Write the report JSON (the dashboard reads this)
  -k, --top <n>           Rows per ranking          [default: 25]
  -f, --format <name>     auto | spotify-extended | spotify-history |
                          lastfm-dataset | lastfm-api                [default: auto]
  -u, --user <name>       User label for exports that carry none  [default: me]
  -w, --time-weight <x>   0 = rank by plays, 1 = rank by listening time [default: 0.5]
  -g, --session-gap <min> Minutes of silence that end a session    [default: 30]
  -t, --trend-days <n>    Length of each trend comparison window    [default: 28]
  -q, --quiet             Suppress the console summary
  -h, --help              Show this message
)";
}

Format format_from_name(const std::string& s, bool* ok) {
  *ok = true;
  if (s == "auto") return Format::kAuto;
  if (s == "spotify-extended") return Format::kSpotifyExtended;
  if (s == "spotify-history") return Format::kSpotifyHistory;
  if (s == "lastfm-dataset") return Format::kLastfmDataset;
  if (s == "lastfm-api") return Format::kLastfmApi;
  *ok = false;
  return Format::kAuto;
}

std::string humanize_minutes(std::uint64_t ms) {
  const double minutes = static_cast<double>(ms) / 60000.0;
  char buf[64];
  if (minutes >= 60) {
    std::snprintf(buf, sizeof(buf), "%.1f h", minutes / 60.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f min", minutes);
  }
  return buf;
}

// printf's %-Ns pads by bytes, which misaligns any row containing non-ASCII
// text — and artist and track names very often do. These two helpers count and
// cut on UTF-8 character boundaries instead.
std::size_t display_width(const std::string& s) {
  std::size_t n = 0;
  for (char ch : s) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;  // count lead bytes only
  }
  return n;
}

std::string fit(std::string s, std::size_t width) {
  if (display_width(s) > width) {
    std::size_t chars = 0, cut = s.size();
    for (std::size_t i = 0; i < s.size(); ++i) {
      if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
        if (chars == width - 3) { cut = i; break; }
        ++chars;
      }
    }
    s = s.substr(0, cut) + "...";
  }
  s.append(width - display_width(s), ' ');
  return s;
}

void print_table(const char* title, const std::vector<RankedRow>& rows, std::size_t limit,
                 bool show_momentum) {
  if (rows.empty()) return;
  constexpr std::size_t kNameWidth = 38;
  std::printf("\n%s\n", title);
  std::printf("  %-3s %s %8s %11s%s\n", "#", fit("name", kNameWidth).c_str(), "plays", "listened",
              show_momentum ? "   momentum" : "");
  for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
    const RankedRow& r = rows[i];
    std::string label = r.name;
    if (!r.secondary.empty()) label += " — " + r.secondary;
    std::printf("  %-3zu %s %8llu %11s", i + 1, fit(label, kNameWidth).c_str(),
                static_cast<unsigned long long>(r.plays), humanize_minutes(r.ms).c_str());
    if (show_momentum) std::printf("   %+.2f", r.momentum);
    std::printf("\n");
  }
}

void print_hours(const std::array<std::uint64_t, 24>& hours) {
  std::uint64_t peak = 0;
  for (std::uint64_t v : hours) peak = std::max(peak, v);
  if (peak == 0) return;
  std::printf("\nListening by hour (UTC)\n");
  static const char* kBlocks[] = {" ", "▁", "▂", "▃",
                                  "▄", "▅", "▆", "▇"};
  std::printf("  ");
  for (std::uint64_t v : hours) {
    const std::size_t level = v == 0 ? 0 : 1 + (v * 6) / peak;
    std::printf("%s", kBlocks[std::min<std::size_t>(level, 7)]);
  }
  std::printf("\n  0h      6h      12h      18h     23h   (peak %llu plays)\n",
              static_cast<unsigned long long>(peak));
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string out_path;
  std::string user = "me";
  Format fmt = Format::kAuto;
  EngineConfig cfg;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "sae: " << what << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "-o" || a == "--out") out_path = need("--out");
    else if (a == "-k" || a == "--top") cfg.top_k = std::stoul(need("--top"));
    else if (a == "-u" || a == "--user") user = need("--user");
    else if (a == "-w" || a == "--time-weight") cfg.time_weight = std::stod(need("--time-weight"));
    else if (a == "-g" || a == "--session-gap") cfg.session_gap_sec = std::stoll(need("--session-gap")) * 60;
    else if (a == "-t" || a == "--trend-days") cfg.trend_window_days = std::stoi(need("--trend-days"));
    else if (a == "-q" || a == "--quiet") quiet = true;
    else if (a == "-f" || a == "--format") {
      bool ok = false;
      fmt = format_from_name(need("--format"), &ok);
      if (!ok) { std::cerr << "sae: unknown format\n"; return 2; }
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "sae: unknown option " << a << "\n";
      return 2;
    } else {
      inputs.push_back(a);
    }
  }

  if (inputs.empty()) { usage(); return 2; }
  if (cfg.time_weight < 0 || cfg.time_weight > 1) {
    std::cerr << "sae: --time-weight must be between 0 and 1\n";
    return 2;
  }

  Catalog cat;
  std::vector<StreamEvent> events;
  IngestStats total;

  const auto t0 = std::chrono::steady_clock::now();
  for (const std::string& path : inputs) {
    std::string error;
    const IngestStats st = ingest_path(path, fmt, user, &cat, &events, &error);
    if (!error.empty()) {
      std::cerr << "sae: " << error << "\n";
      if (st.events_emitted == 0) return 1;
    }
    if (!quiet) {
      std::printf("read %-44s %8zu events\n", path.c_str(), st.events_emitted);
    }
    total.merge(st);
  }
  const auto t1 = std::chrono::steady_clock::now();

  if (events.empty()) {
    std::cerr << "sae: no usable events found\n";
    return 1;
  }

  Report rep = run_pipeline(events, cat, cfg);
  rep.ingest_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!quiet) {
    std::printf("\n%llu events · %llu artists · %llu tracks · %s listened\n",
                static_cast<unsigned long long>(rep.total_events),
                static_cast<unsigned long long>(rep.distinct_artists),
                static_cast<unsigned long long>(rep.distinct_tracks),
                humanize_minutes(rep.total_ms).c_str());
    std::printf("%llu sessions, %.1f plays each on average · %.1f%% skipped\n",
                static_cast<unsigned long long>(rep.sessions.count), rep.sessions.mean_events,
                rep.skip_rate * 100.0);
    std::printf("ingest %.1f ms · aggregate %.1f ms · rank %.1f ms\n", rep.ingest_ms,
                rep.aggregate_ms, rep.rank_ms);

    print_table("Top artists", rep.top_artists, 10, false);
    print_table("Top tracks", rep.top_tracks, 10, false);
    print_table("Rising", rep.rising, 5, true);
    print_table("Falling", rep.falling, 5, true);
    print_hours(rep.hour_plays);
  }

  if (!out_path.empty()) {
    const std::string doc = report_to_json(rep, total, cfg);
    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
      std::cerr << "sae: cannot write " << out_path << "\n";
      return 1;
    }
    f << doc;
    if (!quiet) std::printf("\nwrote %s (%zu bytes)\n", out_path.c_str(), doc.size());
  }
  return 0;
}
