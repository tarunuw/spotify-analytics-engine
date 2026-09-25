// A dependency-free JSON layer with two modes, because the two real sources
// have opposite shapes:
//
//   * Spotify's "Extended Streaming History" is one enormous array of flat
//     objects. Building a DOM for it would cost hundreds of MB of nodes for
//     data we read once, so `JsonScanner` walks it as a token stream and lets
//     the parser pull only the eight fields it needs.
//   * Last.fm's API returns small, deeply nested pages. There a DOM is simpler
//     and the size is bounded by the page limit, so `JsonValue` is used.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sae::json {

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& what, std::size_t byte_offset)
      : std::runtime_error(what + " at byte " + std::to_string(byte_offset)),
        offset(byte_offset) {}
  std::size_t offset;
};

// ---------------------------------------------------------------------------
// Shared scalar helpers
// ---------------------------------------------------------------------------

// Decodes a JSON string body (the bytes between the quotes) into `out`,
// resolving escapes and encoding \uXXXX (with surrogate pairs) as UTF-8.
void unescape(std::string_view raw, std::string* out);

// ---------------------------------------------------------------------------
// Streaming scanner
// ---------------------------------------------------------------------------

enum class Token : std::uint8_t {
  kEnd,
  kObjectBegin,
  kObjectEnd,
  kArrayBegin,
  kArrayEnd,
  kKey,      // string immediately followed by ':'
  kString,
  kNumber,
  kTrue,
  kFalse,
  kNull,
};

// A forward-only cursor over a JSON buffer. The caller owns the buffer; tokens
// reference it by view, so nothing is copied until `string_value()` is asked
// for an unescaped copy.
class JsonScanner {
 public:
  explicit JsonScanner(std::string_view buf) : buf_(buf) {}

  // Advances to the next token. Returns kEnd at the end of input.
  Token next();

  Token token() const { return tok_; }
  // Raw (still-escaped) body of the current key or string token.
  std::string_view raw() const { return raw_; }
  // Unescaped copy of the current key or string token.
  std::string string_value() const {
    std::string out;
    unescape(raw_, &out);
    return out;
  }
  // True when the current key/string needs no unescaping, so `raw()` can be
  // compared or interned directly. The common case for these datasets.
  bool raw_is_literal() const { return !escaped_; }
  double number_value() const { return num_; }
  std::int64_t int_value() const { return static_cast<std::int64_t>(num_); }
  std::size_t offset() const { return pos_; }

  // Consumes the value that starts at the current token, including all of its
  // children. Used to step over the ~20 fields of a Spotify row we ignore.
  void skip_value();

 private:
  void skip_ws();
  [[noreturn]] void fail(const std::string& msg) const { throw ParseError(msg, pos_); }

  std::string_view buf_;
  std::size_t pos_ = 0;
  Token tok_ = Token::kEnd;
  std::string_view raw_;
  double num_ = 0;
  bool escaped_ = false;
};

// ---------------------------------------------------------------------------
// DOM
// ---------------------------------------------------------------------------

class JsonValue {
 public:
  enum class Type : std::uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };

  JsonValue() = default;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }

  // Accessors return a benign default rather than throwing: API payloads vary
  // between Last.fm endpoints and a missing optional field is not an error.
  std::string_view as_string(std::string_view fallback = {}) const {
    return type_ == Type::kString ? std::string_view(str_) : fallback;
  }
  double as_number(double fallback = 0) const { return type_ == Type::kNumber ? num_ : fallback; }
  bool as_bool(bool fallback = false) const { return type_ == Type::kBool ? bool_ : fallback; }
  const std::vector<JsonValue>& as_array() const { return arr_; }

  // Object member lookup; returns a static null value when absent.
  const JsonValue& operator[](std::string_view key) const;
  // Array index; returns a static null value when out of range.
  const JsonValue& at(std::size_t i) const;
  std::size_t size() const { return type_ == Type::kArray ? arr_.size() : obj_.size(); }

  // Last.fm wraps single-element collections as an object instead of a
  // one-element array. This yields the array either way.
  std::vector<const JsonValue*> as_array_or_single() const;

  static JsonValue parse(std::string_view text);

 private:
  friend class DomBuilder;
  Type type_ = Type::kNull;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<JsonValue> arr_;
  std::map<std::string, JsonValue, std::less<>> obj_;
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

// Minimal streaming writer for the report. Tracks nesting so commas and the
// key/value alternation are correct without the caller managing them.
class JsonWriter {
 public:
  explicit JsonWriter(std::string* out, bool pretty = true) : out_(out), pretty_(pretty) {}

  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view k);
  JsonWriter& value(std::string_view s);
  JsonWriter& value(const char* s) { return value(std::string_view(s)); }
  JsonWriter& value(std::int64_t v);
  JsonWriter& value(double v, int decimals = 4);
  JsonWriter& value(bool v);

  // Covers every other integer type (size_t, uint32_t, uint64_t, int, ...) with
  // one definition. Separate concrete overloads per type broke on macOS: there,
  // unlike Linux/Windows, uint64_t and size_t are genuinely distinct types, so
  // a uint64_t argument had no exact-match overload and every candidate tied on
  // conversion rank, making the call ambiguous.
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
  JsonWriter& value(T v) {
    return value(static_cast<std::int64_t>(v));
  }

  // Convenience: key + value in one call.
  template <typename T>
  JsonWriter& field(std::string_view k, T&& v) {
    key(k);
    return value(std::forward<T>(v));
  }
  JsonWriter& field(std::string_view k, double v, int decimals) {
    key(k);
    return value(v, decimals);
  }

 private:
  void prefix();
  void newline_indent();
  void write_quoted(std::string_view s);

  std::string* out_;
  bool pretty_;
  int depth_ = 0;
  bool need_comma_ = false;
  bool after_key_ = false;
};

}  // namespace sae::json
