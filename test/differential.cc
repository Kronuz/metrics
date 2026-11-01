// Differential and race tests of the atomic counters against frozen copies of the original
// implementation: the compare-exchange saturating counter and the std::lower_bound bucket search.
// Every counter test runs on AtomicCounter and ContendedCounter at 64 bits and at 16 bits, so the
// thresholds of ContendedCounter (2^15 and 2^8 at 16 bits) are crossed by ordinary amounts.
// test/model.cc checks the same counters over every interleaving at 8 bits.
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

// The original AtomicCounter (a compare-exchange loop on one word), on any word.
template <class W> class FrozenAtomicCounter {
  public:
	void add(W amount = 1) noexcept {
		auto value = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(value, W(value + std::min(amount, W(kMax - value))),
		                                     std::memory_order_relaxed)) {
		}
	}
	W get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	static constexpr W kMax = std::numeric_limits<W>::max();
	std::atomic<W> value_{0};
};

static std::size_t frozen_bucket(std::uint64_t ns) {
	return static_cast<std::size_t>(std::lower_bound(duration_bounds_ns.begin(), duration_bounds_ns.end(), ns) -
									duration_bounds_ns.begin());
}

template <class W> constexpr W word_max = std::numeric_limits<W>::max();

// Sanitizer builds run the concurrent tests at an eighth of the effort.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr int effort = 1;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr int effort = 1;
#else
constexpr int effort = 8;
#endif
#else
constexpr int effort = 8;
#endif

template <class W> W random_amount(std::mt19937_64 &rng) {
	switch (rng() % 6) {
		case 0: return 1;
		case 1: return W(rng() % 1000);
		case 2: return W(rng());
		case 3: return W(word_max<W> - rng() % 4);
		case 4: return W(rng() % 16);
		default: return W(rng() >> (rng() % 64));
	}
}

// Sequential behavior equals the frozen counter after every add, including ones that saturate.
template <class C, class W> void sequences() {
	std::mt19937_64 rng(7);
	for (int round = 0; round < 2000; ++round) {
		C a;
		FrozenAtomicCounter<W> b;
		for (int i = 0, n = 1 + int(rng() % 40); i < n; ++i) {
			auto amount = random_amount<W>(rng);
			a.add(amount);
			b.add(amount);
			CHECK(a.get() == b.get());
		}
	}
	// Many small amounts carry a narrow word across the fast-path thresholds, then saturate it.
	for (int round = 0; round < 20 && std::numeric_limits<W>::digits < 64; ++round) {
		C a;
		FrozenAtomicCounter<W> b;
		for (int i = 0; i < 300000 && b.get() != word_max<W>; ++i) {
			auto amount = W(rng() % 300);
			a.add(amount);
			b.add(amount);
			CHECK(a.get() == b.get());
		}
		CHECK(b.get() == word_max<W>);
		// Adds after saturation must leave the value pinned.
		for (int i = 0; i < 2000; ++i) {
			auto amount = W(rng() % 300);
			a.add(amount);
			b.add(amount);
			CHECK(a.get() == word_max<W>);
		}
	}
	C c;
	c.add(word_max<W>);
	CHECK(c.get() == word_max<W>);
	c.add();
	CHECK(c.get() == word_max<W>);
	c.add(0);
	CHECK(c.get() == word_max<W>);
	C z;
	z.add(0);
	CHECK(z.get() == 0);
	C d;
	d.add(word_max<W> - 1);
	d.add(1);
	CHECK(d.get() == word_max<W>);
	d.add(word_max<W>);
	CHECK(d.get() == word_max<W>);
	// observe_max alone keeps the largest value.
	C m;
	W largest = 0;
	for (int i = 0; i < 100; ++i) {
		auto v = random_amount<W>(rng);
		m.observe_max(v);
		largest = std::max(largest, v);
		CHECK(m.get() == largest);
	}
}

// Exact totals when nothing saturates, pinned at the maximum when it does.
template <class C, class W> void concurrent_totals() {
	constexpr bool wide = std::numeric_limits<W>::digits == 64;
	const int per_thread = wide ? 20000 : 1500;
	for (int round = 0; round < 2 * effort; ++round) {
		C exact, sat;
		std::vector<std::thread> ts;
		for (int t = 0; t < 8; ++t) {
			ts.emplace_back([&, t] {
				for (int i = 0; i < per_thread; ++i) {
					exact.add(W(t + 1));
					sat.add(W(word_max<W> / 4));
				}
			});
		}
		for (auto &t : ts) {
			t.join();
		}
		CHECK(exact.get() == W(per_thread * 36ull));
		CHECK(sat.get() == word_max<W>);
	}
}

// Once pinned, concurrent adds never expose a smaller value.
template <class C, class W> void pinned_never_dips() {
	for (W pin : {word_max<W>, W(word_max<W> - 3)}) {
		C c;
		c.add(pin);
		c.add(10);
		CHECK(c.get() == word_max<W>);
		std::atomic<bool> stop{false};
		std::vector<std::thread> ws;
		for (int i = 0; i < 4; ++i) {
			ws.emplace_back([&] {
				while (!stop) {
					c.add(1);
					c.add(word_max<W>);
				}
			});
		}
		long low = 0;
		for (int i = 0; i < 60000 * effort; ++i) {
			low += c.get() != word_max<W>;
		}
		stop = true;
		for (auto &t : ws) {
			t.join();
		}
		CHECK(low == 0 && c.get() == word_max<W>);
	}
}

// Saturating from below under contention: a reader sees a nondecreasing value. Each writer does a
// fixed number of adds (enough to saturate) and the reader reads until all have finished, so the
// result does not depend on how the threads are scheduled.
template <class C, class W> void monotonic_while_saturating() {
	for (int round = 0; round < 20 * effort; ++round) {
		C c;
		std::atomic<bool> go{false};
		std::atomic<int> running{4};
		std::vector<std::thread> ws;
		for (int i = 0; i < 4; ++i) {
			ws.emplace_back([&] {
				while (!go.load(std::memory_order_acquire)) {
				}
				for (int k = 0; k < 2000; ++k) {
					c.add(W(word_max<W> / 3));
				}
				running.fetch_sub(1);
			});
		}
		go.store(true, std::memory_order_release);
		W prev = 0;
		long regress = 0;
		while (running.load() > 0) {
			auto v = c.get();
			regress += v < prev;
			prev = v;
		}
		for (auto &t : ws) {
			t.join();
		}
		CHECK(regress == 0 && c.get() == word_max<W>);
	}
}

// A small add in flight while another thread's large add pins the counter (the race of review
// finding CA2): the final value is the maximum and a reader never sees the value fall. With a
// fetch_add that is allowed to wrap this fails on about every trial on arm and one in a hundred
// on x86.
template <class C, class W> void small_adds_racing_a_pin() {
	constexpr int threads = 4;
	const int trials = 250 * effort;
	for (int t = 0; t < trials; ++t) {
		C c;
		std::atomic<int> go{0};
		std::atomic<bool> stop{false};
		std::vector<std::thread> ts;
		for (int i = 0; i < threads; ++i) {
			ts.emplace_back([&] {
				while (!go.load(std::memory_order_acquire)) {
				}
				for (int k = 0; k < 200; ++k) {
					c.add(1);
				}
			});
		}
		std::thread big([&] {
			while (!go.load(std::memory_order_acquire)) {
			}
			for (int k = 0; k < (t % 50); ++k) {
				std::atomic_signal_fence(std::memory_order_seq_cst);
			}
			c.add(word_max<W>);
		});
		long regress = 0;
		std::thread reader([&] {
			W prev = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				auto v = c.get();
				regress += v < prev;
				prev = v;
			}
		});
		go.store(1, std::memory_order_release);
		for (auto &th : ts) {
			th.join();
		}
		big.join();
		stop = true;
		reader.join();
		CHECK(c.get() == word_max<W>);
		CHECK(regress == 0);
	}
}

template <class C, class W> void run_counter(const char *name) {
	sequences<C, W>();
	concurrent_totals<C, W>();
	pinned_never_dips<C, W>();
	monotonic_while_saturating<C, W>();
	small_adds_racing_a_pin<C, W>();
	std::printf("%s ok\n", name);
}

int main() {
	static_assert(sizeof(AtomicCounter) == 8 && sizeof(Counter) == 8);
	run_counter<AtomicCounter, std::uint64_t>("AtomicCounter (64 bits)");
	run_counter<ContendedCounter, std::uint64_t>("ContendedCounter (64 bits)");
	run_counter<BasicAtomicCounter<std::uint16_t, std::atomic>, std::uint16_t>("AtomicCounter (16 bits)");
	run_counter<BasicContendedCounter<std::uint16_t, std::atomic>, std::uint16_t>("ContendedCounter (16 bits)");

	// Bucket search: every edge, its neighbours and random values against lower_bound; both counter types.
	std::mt19937_64 rng(11);
	std::vector<std::uint64_t> values{0, 1, std::numeric_limits<std::uint64_t>::max(),
	                                  std::numeric_limits<std::uint64_t>::max() - 1};
	for (auto edge : duration_bounds_ns) {
		for (int d = -2; d <= 2; ++d) {
			values.push_back(edge + d);
		}
	}
	for (int i = 0; i < 200000; ++i) {
		values.push_back(rng() >> (rng() % 64));
	}
	for (auto v : values) {
		DurationHistogram<> h;
		AtomicDurationHistogram a;
		ContendedDurationHistogram c;
		h.observe(v);
		a.observe(v);
		c.observe(v);
		for (auto s : {h.snapshot(), a.snapshot(), c.snapshot()}) {
			CHECK(s.buckets[frozen_bucket(v)] == 1 && s.count == 1 && s.sum_ns == v && s.maximum_ns == v);
		}
	}
	std::puts("metrics differential tests passed");
}
