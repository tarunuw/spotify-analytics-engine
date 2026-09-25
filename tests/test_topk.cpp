#include <algorithm>
#include <random>
#include <vector>

#include "sae/topk.hpp"
#include "test_framework.hpp"

using namespace sae;

namespace {

Ranked mk(std::uint32_t key, double score) {
  Ranked r;
  r.key = key;
  r.score = score;
  r.tie = key;
  return r;
}

}  // namespace

TEST(topk, returns_best_first) {
  TopK h(3);
  for (double s : {1.0, 9.0, 5.0, 7.0, 2.0}) h.offer(mk(static_cast<std::uint32_t>(s), s));
  const std::vector<Ranked> out = h.take();
  CHECK_EQ(out.size(), static_cast<std::size_t>(3));
  CHECK_NEAR(out[0].score, 9.0, 1e-9);
  CHECK_NEAR(out[1].score, 7.0, 1e-9);
  CHECK_NEAR(out[2].score, 5.0, 1e-9);
}

TEST(topk, handles_fewer_candidates_than_k) {
  TopK h(10);
  h.offer(mk(1, 3.0));
  h.offer(mk(2, 1.0));
  const std::vector<Ranked> out = h.take();
  CHECK_EQ(out.size(), static_cast<std::size_t>(2));
  CHECK_EQ(out[0].key, 1u);
}

TEST(topk, k_zero_keeps_nothing) {
  TopK h(0);
  for (int i = 0; i < 100; ++i) h.offer(mk(static_cast<std::uint32_t>(i), static_cast<double>(i)));
  CHECK_EQ(h.take().size(), static_cast<std::size_t>(0));
}

TEST(topk, ties_break_deterministically_by_key) {
  // Hash-map iteration order varies between runs; the tie-breaker is what makes
  // the ranking reproducible.
  TopK a(3), b(3);
  const std::vector<std::uint32_t> forward = {1, 2, 3, 4};
  std::vector<std::uint32_t> reverse = forward;
  std::reverse(reverse.begin(), reverse.end());
  for (std::uint32_t k : forward) a.offer(mk(k, 5.0));
  for (std::uint32_t k : reverse) b.offer(mk(k, 5.0));
  const std::vector<Ranked> ra = a.take(), rb = b.take();
  CHECK_EQ(ra.size(), rb.size());
  for (std::size_t i = 0; i < ra.size(); ++i) CHECK_EQ(ra[i].key, rb[i].key);
  CHECK_EQ(ra[0].key, 1u);  // lowest key wins a tie
}

TEST(topk, matches_a_full_sort_on_random_input) {
  // The property that matters: bounded selection must agree exactly with the
  // O(N log N) reference it replaces.
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> dist(0.0, 1000.0);

  for (std::size_t k : {1u, 7u, 50u, 500u}) {
    std::vector<Ranked> all;
    all.reserve(2000);
    TopK heap(k);
    for (std::uint32_t i = 0; i < 2000; ++i) {
      const Ranked r = mk(i, dist(rng));
      all.push_back(r);
      heap.offer(r);
    }
    std::sort(all.begin(), all.end(), RankBetter{});
    const std::vector<Ranked> got = heap.take();
    CHECK_EQ(got.size(), std::min<std::size_t>(k, all.size()));
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK_EQ(got[i].key, all[i].key);
      CHECK_NEAR(got[i].score, all[i].score, 1e-12);
    }
  }
}

TEST(topk, is_order_independent) {
  std::mt19937 rng(99);
  std::vector<Ranked> items;
  for (std::uint32_t i = 0; i < 500; ++i) items.push_back(mk(i, static_cast<double>((i * 37) % 101)));

  TopK first(10);
  for (const Ranked& r : items) first.offer(r);
  const std::vector<Ranked> a = first.take();

  std::shuffle(items.begin(), items.end(), rng);
  TopK second(10);
  for (const Ranked& r : items) second.offer(r);
  const std::vector<Ranked> b = second.take();

  CHECK_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) CHECK_EQ(a[i].key, b[i].key);
}
