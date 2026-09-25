// Bounded top-K selection.
//
// The naive way to find the top 25 artists out of N distinct artists is to sort
// all N and take a prefix: O(N log N) time and O(N) extra space. This keeps a
// min-heap capped at K instead, so each candidate costs at most O(log K) and
// most candidates cost a single comparison against the current worst survivor.
// That is O(N log K) time and O(K) space, and it is what makes the ranking
// stage cheap enough to run over every dimension at once.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sae {

// A scored entity. `key` is the interned id; `tie` breaks score ties so results
// are deterministic regardless of hash-map iteration order.
struct Ranked {
  std::uint32_t key = 0;
  double score = 0;
  std::uint64_t plays = 0;
  std::uint64_t ms = 0;
  std::uint32_t tie = 0;
};

// Orders by score descending, then by `tie` ascending. Used as the "better"
// relation; the heap stores its inverse so the weakest survivor is on top.
struct RankBetter {
  bool operator()(const Ranked& a, const Ranked& b) const {
    if (a.score != b.score) return a.score > b.score;
    return a.tie < b.tie;
  }
};

class TopK {
 public:
  explicit TopK(std::size_t k) : k_(k) { heap_.reserve(k + 1); }

  // O(1) when the candidate cannot beat the current worst, O(log K) otherwise.
  void offer(const Ranked& r) {
    if (k_ == 0) return;
    if (heap_.size() < k_) {
      heap_.push_back(r);
      std::push_heap(heap_.begin(), heap_.end(), RankBetter{});
      return;
    }
    // Under RankBetter-as-less-than, "greatest" means least good, so the root
    // is the weakest survivor and the first candidate for eviction.
    if (!RankBetter{}(r, heap_.front())) return;
    std::pop_heap(heap_.begin(), heap_.end(), RankBetter{});
    heap_.back() = r;
    std::push_heap(heap_.begin(), heap_.end(), RankBetter{});
  }

  // Drains the heap into a best-first vector. O(K log K), once per stage.
  std::vector<Ranked> take() {
    std::vector<Ranked> out = std::move(heap_);
    heap_.clear();
    std::sort(out.begin(), out.end(), RankBetter{});
    return out;
  }

  std::size_t size() const { return heap_.size(); }
  std::size_t capacity() const { return k_; }

 private:
  std::size_t k_;
  std::vector<Ranked> heap_;
};

}  // namespace sae
