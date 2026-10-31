// Differential tests against frozen copies of the original implementation: the
// CAS-loop saturating AtomicCounter and the std::lower_bound bucket search.
#include "metrics.hh"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <thread>
#include <vector>

using namespace kronuz::metrics;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::abort();                                                        \
		}                                                                        \
	} while (0)

constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();

class FrozenAtomicCounter {
  public:
	void add(std::uint64_t amount = 1) noexcept {
		auto value = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(value, value + std::min(amount, kMax - value), std::memory_order_relaxed)) {
		}
	}
	std::uint64_t get() const noexcept { return value_.load(std::memory_order_relaxed); }
  private:
	std::atomic<std::uint64_t> value_{0};
};

static std::size_t frozen_bucket(std::uint64_t ns) {
	return static_cast<std::size_t>(std::lower_bound(duration_bounds_ns.begin(), duration_bounds_ns.end(), ns) -
									duration_bounds_ns.begin());
}

int main() {
	std::mt19937_64 rng(7);
	// Counter: random sequences of adds, from tiny to near-overflow, on both.
	for (int round = 0; round < 2000; ++round) {
		AtomicCounter a;
		FrozenAtomicCounter b;
		for (int i = 0, n = 1 + int(rng() % 40); i < n; ++i) {
			std::uint64_t amount;
			switch (rng() % 5) {
				case 0: amount = 1; break;
				case 1: amount = rng() % 1000; break;
				case 2: amount = rng(); break;
				case 3: amount = kMax - rng() % 4; break;
				default: amount = rng() >> (rng() % 64); break;
			}
			a.add(amount);
			b.add(amount);
			CHECK(a.get() == b.get());
		}
	}
	{
		AtomicCounter c;
		c.add(kMax); CHECK(c.get() == kMax);
		c.add(); CHECK(c.get() == kMax);
		c.add(0); CHECK(c.get() == kMax);
		AtomicCounter z; z.add(0); CHECK(z.get() == 0);
		AtomicCounter d; d.add(kMax - 1); d.add(1); CHECK(d.get() == kMax); d.add(kMax); CHECK(d.get() == kMax);
	}

	// Bucket search: every edge, its neighbours and random values against lower_bound.
	std::vector<std::uint64_t> values{0, 1, kMax, kMax - 1};
	for (auto edge : duration_bounds_ns) for (int d = -2; d <= 2; ++d) values.push_back(edge + d);
	for (int i = 0; i < 200000; ++i) values.push_back(rng() >> (rng() % 64));
	for (auto v : values) {
		DurationHistogram<> h;
		h.observe(v);
		auto s = h.snapshot();
		CHECK(s.buckets[frozen_bucket(v)] == 1 && s.count == 1 && s.sum_ns == v && s.maximum_ns == v);
	}

	// Concurrent: exact totals when nothing saturates, pinned at the maximum when it does.
	for (int round = 0; round < 50; ++round) {
		AtomicCounter exact, sat;
		std::vector<std::thread> ts;
		for (int t = 0; t < 8; ++t) ts.emplace_back([&, t] {
			for (int i = 0; i < 20000; ++i) { exact.add(t + 1); sat.add(kMax / 4); }
		});
		for (auto& t : ts) t.join();
		CHECK(exact.get() == 20000ull * 36);
		CHECK(sat.get() == kMax);
	}
	std::puts("metrics differential tests passed");
}
