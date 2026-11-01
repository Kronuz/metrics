// Contended AtomicCounter and ContendedCounter add against the original CAS-loop saturating
// add and a bare fetch_add (comparator, no saturation). ns per add per thread, best of 5.
#include "metrics.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

class CasCounter {
  public:
	void add(std::uint64_t amount = 1) noexcept {
		constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
		auto value = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(value, value + std::min(amount, maximum - value), std::memory_order_relaxed)) {
		}
	}
	std::uint64_t get() const { return value_.load(); }
  private:
	std::atomic<std::uint64_t> value_{0};
};
class FetchAddCounter {
  public:
	void add(std::uint64_t amount = 1) noexcept { value_.fetch_add(amount, std::memory_order_relaxed); }
	std::uint64_t get() const { return value_.load(); }
  private:
	std::atomic<std::uint64_t> value_{0};
};

template <class C>
static double run(int threads) {
	constexpr int iters = 2000000;
	double best = 1e18;
	for (int rep = 0; rep < 5; ++rep) {
		C c;
		auto a = std::chrono::steady_clock::now();
		std::vector<std::thread> ts;
		for (int t = 0; t < threads; ++t) ts.emplace_back([&] { for (int i = 0; i < iters; ++i) c.add(); });
		for (auto& t : ts) t.join();
		best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count() / iters);
		if (c.get() != std::uint64_t(iters) * threads) std::puts("MISMATCH");
	}
	return best;
}

int main() {
	for (int t : {1, 2, 4, 8})
		std::printf("threads=%d  CAS loop %6.1f   AtomicCounter %6.1f   ContendedCounter %6.1f   bare fetch_add %6.1f  ns/add\n",
		            t, run<CasCounter>(t), run<kronuz::metrics::AtomicCounter>(t),
		            run<kronuz::metrics::ContendedCounter>(t), run<FetchAddCounter>(t));
}
