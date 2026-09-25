#include <string>

#include "sae/json.hpp"
#include "json_shape.hpp"
#include "test_framework.hpp"

using namespace sae::json;

TEST(json, scanner_walks_flat_object) {
  JsonScanner sc(R"({"a":1,"b":"two","c":true,"d":null})");
  CHECK(sc.next() == Token::kObjectBegin);
  CHECK(sc.next() == Token::kKey);
  CHECK_EQ(std::string(sc.raw()), std::string("a"));
  CHECK(sc.next() == Token::kNumber);
  CHECK_EQ(sc.int_value(), static_cast<std::int64_t>(1));
  CHECK(sc.next() == Token::kKey);
  CHECK(sc.next() == Token::kString);
  CHECK_EQ(std::string(sc.raw()), std::string("two"));
  CHECK(sc.next() == Token::kKey);
  CHECK(sc.next() == Token::kTrue);
  CHECK(sc.next() == Token::kKey);
  CHECK(sc.next() == Token::kNull);
  CHECK(sc.next() == Token::kObjectEnd);
  CHECK(sc.next() == Token::kEnd);
}

TEST(json, skip_value_steps_over_nested_containers) {
  // The Spotify parser leans on this to ignore fields it does not read.
  JsonScanner sc(R"({"skip":{"a":[1,2,{"b":3}],"c":"x"},"keep":42})");
  CHECK(sc.next() == Token::kObjectBegin);
  CHECK(sc.next() == Token::kKey);
  sc.next();          // the nested object
  sc.skip_value();    // ... consumed whole
  CHECK(sc.next() == Token::kKey);
  CHECK_EQ(std::string(sc.raw()), std::string("keep"));
  CHECK(sc.next() == Token::kNumber);
  CHECK_EQ(sc.int_value(), static_cast<std::int64_t>(42));
}

TEST(json, skip_value_on_scalar_does_not_advance) {
  JsonScanner sc(R"([1,2])");
  CHECK(sc.next() == Token::kArrayBegin);
  CHECK(sc.next() == Token::kNumber);
  sc.skip_value();
  CHECK(sc.token() == Token::kNumber);
  CHECK_EQ(sc.int_value(), static_cast<std::int64_t>(1));
}

TEST(json, unescape_handles_escapes_and_surrogates) {
  std::string out;
  unescape(R"(line\nbreak \"q\" \\ é)", &out);
  CHECK_EQ(out, std::string("line\nbreak \"q\" \\ \xc3\xa9"));

  // A surrogate pair must recombine into one 4-byte code point (U+1F3B5).
  unescape("\\ud83c\\udfb5", &out);
  CHECK_EQ(out, std::string("\xf0\x9f\x8e\xb5"));

  unescape("\\ud800", &out);  // unpaired high surrogate -> U+FFFD
  CHECK_EQ(out, std::string("\xef\xbf\xbd"));
}

TEST(json, scanner_flags_literal_strings) {
  JsonScanner sc(R"(["plain","esc\ttab"])");
  sc.next();
  sc.next();
  CHECK(sc.raw_is_literal());
  sc.next();
  CHECK(!sc.raw_is_literal());
  CHECK_EQ(sc.string_value(), std::string("esc\ttab"));
}

TEST(json, dom_reads_nested_lastfm_shape) {
  const JsonValue v = JsonValue::parse(
      R"({"recenttracks":{"track":[{"name":"Song","artist":{"#text":"Band"}}],
          "@attr":{"user":"tarun","total":"1"}}})");
  const JsonValue& rt = v["recenttracks"];
  CHECK_EQ(std::string(rt["@attr"]["user"].as_string()), std::string("tarun"));
  CHECK_EQ(rt["track"].size(), static_cast<std::size_t>(1));
  CHECK_EQ(std::string(rt["track"].at(0)["artist"]["#text"].as_string()), std::string("Band"));
}

TEST(json, dom_missing_keys_return_defaults) {
  const JsonValue v = JsonValue::parse(R"({"a":1})");
  CHECK(v["nope"].is_null());
  CHECK(v["nope"]["deeper"].is_null());
  CHECK_EQ(std::string(v["nope"].as_string("fallback")), std::string("fallback"));
  CHECK_NEAR(v["a"].as_number(), 1.0, 1e-9);
}

TEST(json, as_array_or_single_normalises_lastfm_collections) {
  // Last.fm returns an object, not an array, when a page holds one track.
  const JsonValue one = JsonValue::parse(R"({"track":{"name":"Solo"}})");
  CHECK_EQ(one["track"].as_array_or_single().size(), static_cast<std::size_t>(1));
  const JsonValue many = JsonValue::parse(R"({"track":[{"name":"A"},{"name":"B"}]})");
  CHECK_EQ(many["track"].as_array_or_single().size(), static_cast<std::size_t>(2));
  const JsonValue none = JsonValue::parse(R"({})");
  CHECK_EQ(none["track"].as_array_or_single().size(), static_cast<std::size_t>(0));
}

TEST(json, writer_round_trips_through_the_parser) {
  std::string out;
  JsonWriter w(&out);
  w.begin_object();
  w.field("name", "Bon Iver");
  w.field("plays", static_cast<std::int64_t>(12));
  w.field("score", 0.5, 3);
  w.field("ok", true);
  w.key("tags").begin_array().value("folk").value("indie").end_array();
  w.end_object();

  const JsonValue v = JsonValue::parse(out);
  CHECK_EQ(std::string(v["name"].as_string()), std::string("Bon Iver"));
  CHECK_NEAR(v["plays"].as_number(), 12.0, 1e-9);
  CHECK_NEAR(v["score"].as_number(), 0.5, 1e-9);
  CHECK(v["ok"].as_bool());
  CHECK_EQ(v["tags"].size(), static_cast<std::size_t>(2));
}

TEST(json, writer_emits_exactly_one_separator_between_items) {
  std::string out;
  JsonWriter w(&out);
  w.begin_object();
  w.field("a", static_cast<std::int64_t>(1));
  w.field("b", "two");
  w.key("nested").begin_object().field("c", true).end_object();
  w.key("list").begin_array();
  w.begin_object().field("d", 1.5, 2).end_object();
  w.begin_object().field("d", 2.5, 2).end_object();
  w.end_array();
  w.field("last", static_cast<std::int64_t>(9));
  w.end_object();

  const std::string defect = ::testing::separator_defect(out);
  if (!defect.empty()) {
    ::testing::report_failure(__FILE__, __LINE__, "writer output is well-formed", defect + "\n" + out);
  }
  CHECK(out.front() == '{');  // no blank line before the root token
}

TEST(json, writer_escapes_control_and_quote_characters) {
  std::string out;
  JsonWriter w(&out, /*pretty=*/false);
  w.begin_object().field("t", "say \"hi\"\n\tdone").end_object();
  const JsonValue v = JsonValue::parse(out);
  CHECK_EQ(std::string(v["t"].as_string()), std::string("say \"hi\"\n\tdone"));
}

TEST(json, malformed_input_throws_rather_than_guessing) {
  bool threw = false;
  try {
    JsonScanner sc(R"({"a": @})");
    while (sc.next() != Token::kEnd) {
    }
  } catch (const ParseError&) {
    threw = true;
  }
  CHECK(threw);
}
