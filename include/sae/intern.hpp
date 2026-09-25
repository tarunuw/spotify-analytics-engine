// String interning table.
//
// Streaming datasets repeat the same few thousand artist/track/album strings
// across millions of rows. Interning collapses each distinct string to a dense
// 32-bit id exactly once, so every downstream aggregation key is an integer
// compare instead of a string compare, and the hash maps stay cache-friendly.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sae {

using Id = std::uint32_t;
inline constexpr Id kInvalidId = 0xFFFFFFFFu;

class Interner {
 public:
  Interner() { table_.reserve(1u << 14); }

  // Returns the dense id for `s`, inserting it on first sight.
  Id intern(std::string_view s) {
    auto it = table_.find(s);
    if (it != table_.end()) return it->second;
    const Id id = static_cast<Id>(values_.size());
    values_.emplace_back(s);
    // Key the map at the owned copy so the view never dangles.
    table_.emplace(std::string_view(values_.back()), id);
    return id;
  }

  // Returns kInvalidId when `s` was never interned.
  Id lookup(std::string_view s) const {
    auto it = table_.find(s);
    return it == table_.end() ? kInvalidId : it->second;
  }

  const std::string& str(Id id) const { return values_[id]; }
  std::size_t size() const { return values_.size(); }

 private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      // FNV-1a: cheap, well-distributed for short text keys.
      std::uint64_t h = 1469598103934665603ull;
      for (char raw_c : s) {
        h ^= static_cast<unsigned char>(raw_c);
        h *= 1099511628211ull;
      }
      return static_cast<std::size_t>(h);
    }
  };
  struct Eq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  // A deque, not a vector: push_back never relocates existing elements, so the
  // string_view keys held by `table_` stay valid for the life of the interner.
  std::deque<std::string> values_;
  std::unordered_map<std::string_view, Id, Hash, Eq> table_;
};

}  // namespace sae
