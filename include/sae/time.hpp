// Timestamp helpers.
//
// Every source hands us UTC. We convert to epoch seconds ourselves rather than
// calling timegm/strptime so the engine is deterministic across platforms and
// independent of the host's TZ database.
#pragma once

#include <cstdint>
#include <string_view>

namespace sae {

// Howard Hinnant's days_from_civil. Valid for any year in the proleptic
// Gregorian calendar; branch-free apart from the leap-era arithmetic.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of the above, used to bucket events into calendar days/weeks.
struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + (m <= 2), m, d};
}

// Parses "YYYY-MM-DDTHH:MM:SS[Z]" (Spotify `ts`, Last.fm dataset column 2).
// Returns false on any shape it does not recognise rather than guessing.
inline bool parse_iso8601_utc(std::string_view s, std::int64_t* out) noexcept {
  if (s.size() < 19) return false;
  auto num = [&](std::size_t off, std::size_t len, int* dst) {
    int v = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const char c = s[off + i];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    *dst = v;
    return true;
  };
  int Y, M, D, h, mi, sec;
  if (!num(0, 4, &Y) || !num(5, 2, &M) || !num(8, 2, &D)) return false;
  if (!num(11, 2, &h) || !num(14, 2, &mi) || !num(17, 2, &sec)) return false;
  if (s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ')) return false;
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || sec > 60) return false;
  *out = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D)) * 86400 +
         h * 3600 + mi * 60 + sec;
  return true;
}

// 0 = Sunday .. 6 = Saturday. 1970-01-01 was a Thursday (weekday 4).
inline unsigned weekday_utc(std::int64_t epoch_sec) noexcept {
  std::int64_t days = epoch_sec / 86400;
  if (epoch_sec % 86400 < 0) --days;
  return static_cast<unsigned>(((days % 7) + 11) % 7);
}

inline unsigned hour_utc(std::int64_t epoch_sec) noexcept {
  std::int64_t s = epoch_sec % 86400;
  if (s < 0) s += 86400;
  return static_cast<unsigned>(s / 3600);
}

inline std::int64_t day_index(std::int64_t epoch_sec) noexcept {
  std::int64_t days = epoch_sec / 86400;
  if (epoch_sec % 86400 < 0) --days;
  return days;
}

}  // namespace sae
