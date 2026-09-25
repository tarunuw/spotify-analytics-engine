// Strict-structure check shared by the writer and report tests.
//
// The scanner treats ',' and ':' as insignificant whitespace, which makes it too
// forgiving to catch a writer that emits a stray or missing separator — a bug
// that only surfaces once the file reaches a strict parser such as the
// dashboard's JSON.parse. This validates the structure the scanner ignores.
#pragma once

#include <string>

namespace testing {

// The scanner treats ',' and ':' as insignificant whitespace, which makes it
// too forgiving to catch a writer that emits a stray or missing separator — a
// bug that only shows up once the file reaches a strict parser. This checks the
// structure the scanner ignores.
inline std::string separator_defect(const std::string& doc) {
  bool in_string = false, escaped = false;
  char last = 0;  // last significant, non-whitespace character
  for (std::size_t i = 0; i < doc.size(); ++i) {
    const char c = doc[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') { in_string = false; last = '"'; }
      continue;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    if (c == '"') {
      if (last == '}' || last == ']' || last == '"') {
        return "value or key at byte " + std::to_string(i) + " has no separator before it";
      }
      in_string = true;
      continue;
    }
    if (c == ',' && (last == ',' || last == '{' || last == '[' || last == ':' || last == 0)) {
      return "stray comma at byte " + std::to_string(i);
    }
    if ((c == '}' || c == ']') && (last == ',' || last == ':')) {
      return "trailing separator before close at byte " + std::to_string(i);
    }
    if (c == ':' && last != '"') {
      return "colon not preceded by a key at byte " + std::to_string(i);
    }
    last = c;
  }
  return {};
}

}  // namespace testing
