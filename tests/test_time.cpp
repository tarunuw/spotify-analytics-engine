#include "sae/time.hpp"
#include "test_framework.hpp"

using namespace sae;

TEST(time, epoch_and_known_dates) {
  std::int64_t t = 0;
  CHECK(parse_iso8601_utc("1970-01-01T00:00:00Z", &t));
  CHECK_EQ(t, static_cast<std::int64_t>(0));

  CHECK(parse_iso8601_utc("2024-02-29T12:34:56Z", &t));  // leap day
  CHECK_EQ(t, static_cast<std::int64_t>(1709210096));

  CHECK(parse_iso8601_utc("2000-02-29T00:00:00Z", &t));  // century leap year
  CHECK_EQ(t, static_cast<std::int64_t>(951782400));

  CHECK(parse_iso8601_utc("2009-05-04T23:08:57Z", &t));  // Last.fm 1K row
  CHECK_EQ(t, static_cast<std::int64_t>(1241478537));
}

TEST(time, accepts_space_separator_and_missing_zulu) {
  std::int64_t a = 0, b = 0;
  CHECK(parse_iso8601_utc("2019-03-04 14:22:00", &a));
  CHECK(parse_iso8601_utc("2019-03-04T14:22:00Z", &b));
  CHECK_EQ(a, b);
}

TEST(time, rejects_malformed_stamps) {
  std::int64_t t = 0;
  CHECK(!parse_iso8601_utc("", &t));
  CHECK(!parse_iso8601_utc("2024-02-29", &t));          // too short
  CHECK(!parse_iso8601_utc("2024/02/29T00:00:00Z", &t));  // wrong separators
  CHECK(!parse_iso8601_utc("2024-13-01T00:00:00Z", &t));  // month 13
  CHECK(!parse_iso8601_utc("2024-01-01T25:00:00Z", &t));  // hour 25
  CHECK(!parse_iso8601_utc("20xx-01-01T00:00:00Z", &t));  // non-digits
}

TEST(time, weekday_and_hour_buckets) {
  std::int64_t t = 0;
  parse_iso8601_utc("1970-01-01T00:00:00Z", &t);
  CHECK_EQ(weekday_utc(t), 4u);  // a Thursday
  parse_iso8601_utc("2026-09-08T21:45:00Z", &t);
  CHECK_EQ(weekday_utc(t), 2u);  // a Tuesday
  CHECK_EQ(hour_utc(t), 21u);
  parse_iso8601_utc("2026-09-08T00:00:00Z", &t);
  CHECK_EQ(hour_utc(t), 0u);
}

TEST(time, civil_round_trip) {
  // Every day across a four-century cycle boundary must survive the round trip.
  for (std::int64_t d = -30000; d < 30000; d += 7) {
    const CivilDate c = civil_from_days(d);
    CHECK_EQ(days_from_civil(c.year, c.month, c.day), d);
  }
}

TEST(time, day_index_is_stable_before_epoch) {
  std::int64_t t = 0;
  parse_iso8601_utc("1969-12-31T23:59:59Z", &t);
  CHECK_EQ(day_index(t), static_cast<std::int64_t>(-1));
  CHECK_EQ(hour_utc(t), 23u);
}
