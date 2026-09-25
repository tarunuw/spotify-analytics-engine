#include "sae/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sae::json {
namespace {

void append_utf8(std::uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::uint32_t read_hex4(std::string_view s, std::size_t i) {
  std::uint32_t v = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    const int d = (i + k < s.size()) ? hex_val(s[i + k]) : -1;
    if (d < 0) return 0xFFFFFFFFu;
    v = v * 16 + static_cast<std::uint32_t>(d);
  }
  return v;
}

}  // namespace

void unescape(std::string_view raw, std::string* out) {
  out->clear();
  out->reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (++i >= raw.size()) break;
    switch (raw[i]) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        std::uint32_t cp = read_hex4(raw, i + 1);
        if (cp == 0xFFFFFFFFu) { out->push_back('?'); break; }
        i += 4;
        // Recombine a surrogate pair; an unpaired surrogate becomes U+FFFD.
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
            const std::uint32_t lo = read_hex4(raw, i + 3);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            } else {
              cp = 0xFFFD;
            }
          } else {
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        append_utf8(cp, out);
        break;
      }
      default: out->push_back(raw[i]); break;
    }
  }
}

// ---------------------------------------------------------------------------
// JsonScanner
// ---------------------------------------------------------------------------

void JsonScanner::skip_ws() {
  while (pos_ < buf_.size()) {
    const char c = buf_[pos_];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++pos_;
    } else if (c == ',' || c == ':') {
      // Structural separators carry no information the caller needs: the token
      // sequence alone determines position within an object or array.
      ++pos_;
    } else {
      return;
    }
  }
}

Token JsonScanner::next() {
  skip_ws();
  if (pos_ >= buf_.size()) return tok_ = Token::kEnd;

  const char c = buf_[pos_];
  switch (c) {
    case '{': ++pos_; return tok_ = Token::kObjectBegin;
    case '}': ++pos_; return tok_ = Token::kObjectEnd;
    case '[': ++pos_; return tok_ = Token::kArrayBegin;
    case ']': ++pos_; return tok_ = Token::kArrayEnd;
    case 't':
      if (buf_.compare(pos_, 4, "true") != 0) fail("expected 'true'");
      pos_ += 4;
      return tok_ = Token::kTrue;
    case 'f':
      if (buf_.compare(pos_, 5, "false") != 0) fail("expected 'false'");
      pos_ += 5;
      return tok_ = Token::kFalse;
    case 'n':
      if (buf_.compare(pos_, 4, "null") != 0) fail("expected 'null'");
      pos_ += 4;
      return tok_ = Token::kNull;
    case '"': {
      const std::size_t start = ++pos_;
      escaped_ = false;
      while (pos_ < buf_.size()) {
        const char s = buf_[pos_];
        if (s == '\\') {
          escaped_ = true;
          pos_ += 2;
          continue;
        }
        if (s == '"') break;
        ++pos_;
      }
      if (pos_ >= buf_.size()) fail("unterminated string");
      raw_ = buf_.substr(start, pos_ - start);
      ++pos_;  // closing quote
      // A ':' after the string (modulo whitespace) makes this an object key.
      std::size_t look = pos_;
      while (look < buf_.size() &&
             (buf_[look] == ' ' || buf_[look] == '\t' || buf_[look] == '\n' || buf_[look] == '\r')) {
        ++look;
      }
      return tok_ = (look < buf_.size() && buf_[look] == ':') ? Token::kKey : Token::kString;
    }
    default: {
      if (c != '-' && (c < '0' || c > '9')) fail(std::string("unexpected character '") + c + "'");
      // Delimit the literal before converting: the buffer is a view and may not
      // be NUL-terminated, so strtod must never be pointed straight at it.
      std::size_t end = pos_;
      while (end < buf_.size()) {
        const char n = buf_[end];
        const bool part = (n >= '0' && n <= '9') || n == '-' || n == '+' || n == '.' || n == 'e' ||
                          n == 'E';
        if (!part) break;
        ++end;
      }
      char tmp[64];
      const std::size_t len = end - pos_;
      if (len == 0 || len >= sizeof(tmp)) fail("malformed number");
      std::memcpy(tmp, buf_.data() + pos_, len);
      tmp[len] = '\0';
      char* last = nullptr;
      num_ = std::strtod(tmp, &last);
      if (last == tmp) fail("malformed number");
      pos_ = end;
      return tok_ = Token::kNumber;
    }
  }
}

void JsonScanner::skip_value() {
  int depth = 0;
  do {
    switch (tok_) {
      case Token::kObjectBegin:
      case Token::kArrayBegin: ++depth; break;
      case Token::kObjectEnd:
      case Token::kArrayEnd: --depth; break;
      case Token::kEnd: return;
      default: break;
    }
    if (depth == 0) return;  // scalar, or the container just closed
    next();
  } while (true);
}

// ---------------------------------------------------------------------------
// DOM
// ---------------------------------------------------------------------------

namespace {
const JsonValue& null_value() {
  static const JsonValue kNull;
  return kNull;
}
}  // namespace

const JsonValue& JsonValue::operator[](std::string_view key) const {
  if (type_ != Type::kObject) return null_value();
  auto it = obj_.find(key);
  return it == obj_.end() ? null_value() : it->second;
}

const JsonValue& JsonValue::at(std::size_t i) const {
  if (type_ != Type::kArray || i >= arr_.size()) return null_value();
  return arr_[i];
}

std::vector<const JsonValue*> JsonValue::as_array_or_single() const {
  std::vector<const JsonValue*> out;
  if (type_ == Type::kArray) {
    out.reserve(arr_.size());
    for (const auto& v : arr_) out.push_back(&v);
  } else if (type_ != Type::kNull) {
    out.push_back(this);
  }
  return out;
}

class DomBuilder {
 public:
  explicit DomBuilder(JsonScanner& sc) : sc_(sc) {}

  JsonValue build() {
    JsonValue v;
    switch (sc_.token()) {
      case Token::kObjectBegin: {
        v.type_ = JsonValue::Type::kObject;
        while (sc_.next() != Token::kObjectEnd) {
          if (sc_.token() == Token::kEnd) throw ParseError("unterminated object", sc_.offset());
          if (sc_.token() != Token::kKey) throw ParseError("expected object key", sc_.offset());
          std::string k = sc_.string_value();
          sc_.next();
          v.obj_.emplace(std::move(k), build());
        }
        break;
      }
      case Token::kArrayBegin: {
        v.type_ = JsonValue::Type::kArray;
        while (sc_.next() != Token::kArrayEnd) {
          if (sc_.token() == Token::kEnd) throw ParseError("unterminated array", sc_.offset());
          v.arr_.push_back(build());
        }
        break;
      }
      case Token::kKey:
      case Token::kString:
        v.type_ = JsonValue::Type::kString;
        v.str_ = sc_.string_value();
        break;
      case Token::kNumber:
        v.type_ = JsonValue::Type::kNumber;
        v.num_ = sc_.number_value();
        break;
      case Token::kTrue:
        v.type_ = JsonValue::Type::kBool;
        v.bool_ = true;
        break;
      case Token::kFalse:
        v.type_ = JsonValue::Type::kBool;
        v.bool_ = false;
        break;
      default: break;  // null
    }
    return v;
  }

 private:
  JsonScanner& sc_;
};

JsonValue JsonValue::parse(std::string_view text) {
  JsonScanner sc(text);
  sc.next();
  DomBuilder b(sc);
  return b.build();
}

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------

void JsonWriter::newline_indent() {
  if (!pretty_) return;
  if (out_->empty()) return;  // no blank first line before the root token
  out_->push_back('\n');
  out_->append(static_cast<std::size_t>(depth_) * 2, ' ');
}

void JsonWriter::prefix() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (need_comma_) out_->push_back(',');
  newline_indent();
}

JsonWriter& JsonWriter::begin_object() {
  prefix();
  out_->push_back('{');
  ++depth_;
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  --depth_;
  newline_indent();
  out_->push_back('}');
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  prefix();
  out_->push_back('[');
  ++depth_;
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  --depth_;
  newline_indent();
  out_->push_back(']');
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
  prefix();
  // Deliberately not `value(k)`: that would run prefix() a second time and emit
  // a stray separator before every key.
  write_quoted(k);
  out_->push_back(':');
  if (pretty_) out_->push_back(' ');
  after_key_ = true;
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view s) {
  prefix();
  write_quoted(s);
  need_comma_ = true;
  return *this;
}

void JsonWriter::write_quoted(std::string_view s) {
  out_->push_back('"');
  for (char raw_c : s) {
    const auto c = static_cast<unsigned char>(raw_c);
    switch (c) {
      case '"': out_->append("\\\""); break;
      case '\\': out_->append("\\\\"); break;
      case '\n': out_->append("\\n"); break;
      case '\r': out_->append("\\r"); break;
      case '\t': out_->append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out_->append(buf);
        } else {
          out_->push_back(static_cast<char>(c));
        }
    }
  }
  out_->push_back('"');
}

JsonWriter& JsonWriter::value(std::int64_t v) {
  prefix();
  out_->append(std::to_string(v));
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(double v, int decimals) {
  prefix();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  out_->append(buf);
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(bool v) {
  prefix();
  out_->append(v ? "true" : "false");
  need_comma_ = true;
  return *this;
}

}  // namespace sae::json
