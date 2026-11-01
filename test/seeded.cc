// The atomic counters at 64 bits, started from words around their thresholds.
//
// A ContendedCounter keeps two words and sends an add to `small_` only after a load below 2^63,
// so a wrap of `small_` needs it near 2^64. Real adds never get it there (it would take 2^31
// adds of 2^32), which is why test/model.cc checks the rule at 8 bits and test/differential.cc at
// 16. Here the counter is built over an atomic that records its words, and the test stores the
// starting values into them, so the 64-bit code is exercised at its thresholds: `small_` just
// below 2^63, at it, and within a few adds of 2^64; `large_` near the maximum; both together.
//
// Every run compares the counter with a 128-bit oracle: after each add (sequential runs) and at
// the end (concurrent runs), the value is min(initial + adds, 2^64 - 1); concurrent readers never
// see it fall or exceed the saturated total of the adds started.
#include "metrics.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace kronuz::metrics;
using u64 = std::uint64_t;
using u128 = unsigned __int128;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::abort();                                                        \
		}                                                                        \
	} while (0)

constexpr u64 maximum = ~u64(0);
constexpr u64 half = u64(1) << 63;

// An atomic that records itself when built, in construction order (a counter's words are its
// members, in declaration order), so a test can store into them.
template <class T> class Seedable : public std::atomic<T> {
  public:
	Seedable(T initial) : std::atomic<T>(initial) { words().push_back(this); }
	static std::vector<std::atomic<T> *> &words() {
		static thread_local std::vector<std::atomic<T> *> list;
		return list;
	}
};

template <class C> class Seeded {
  public:
	// `small` goes into the first word and `large` into the second, when there is one.
	Seeded(u64 small, u64 large) {
		auto &words = Seedable<u64>::words();
		words.clear();
		counter_ = std::make_unique<C>();
		words[0]->store(small);
		if (words.size() > 1) {
			words[1]->store(large);
		}
		initial_ = small + (words.size() > 1 ? u128(large) : u128(0));
	}
	C &operator*() { return *counter_; }
	u64 initial() const { return saturate(initial_); }
	static u64 saturate(u128 v) { return v > maximum ? maximum : u64(v); }

  private:
	std::unique_ptr<C> counter_;
	u128 initial_ = 0;
};

using Atomic64 = BasicAtomicCounter<u64, Seedable>;
using Contended64 = BasicContendedCounter<u64, Seedable>;

const std::vector<u64> seeds{0,        1,        u64(1) << 32, half - 2,     half - 1, half,
                             half + 1, maximum - (u64(1) << 33), maximum - 5, maximum - 1, maximum};
const std::vector<u64> amounts{0, 1, 15, (u64(1) << 32) - 1, u64(1) << 32, u64(1) << 40, half - 1, half, maximum - 1, maximum};

// One thread, every add checked against the oracle, from every pair of starting words.
template <class C> void sequential(const char *name, bool two_words) {
	std::mt19937_64 rng(5);
	long runs = 0;
	for (u64 small : seeds) {
		for (u64 large : seeds) {
			if (!two_words && large != 0) {
				continue;
			}
			for (int round = 0; round < 6; ++round) {
				Seeded<C> seeded(small, large);
				auto &c = *seeded;
				u128 expected = seeded.initial();
				CHECK(c.get() == seeded.initial());
				for (int i = 0; i < 12; ++i) {
					// Mostly amounts below 2^32 (the ones `small_` takes), then the rest.
					u64 amount = rng() % 4 ? amounts[rng() % 5] : amounts[rng() % amounts.size()];
					c.add(amount);
					expected = Seeded<C>::saturate(expected + amount);
					CHECK(c.get() == expected);
				}
				++runs;
			}
		}
	}
	std::printf("%-40s %ld sequences hold\n", name, runs);
}

// Several threads add amounts below 2^32 from a state a few adds short of the wrap, while a reader
// checks the value only ever rises and never exceeds the saturated total of everything started.
template <class C> void concurrent(const char *name, bool two_words) {
	constexpr int threads = 6, per_thread = 400;
	long runs = 0;
	for (u64 small : seeds) {
		for (u64 large : {u64(0), half, maximum - 40000}) {
			if (!two_words && large != 0) {
				continue;
			}
			for (int round = 0; round < 8; ++round) {
				Seeded<C> seeded(small, large);
				auto &c = *seeded;
				u128 total = 0;
				std::vector<std::vector<u64>> adds(threads);
				std::mt19937_64 rng(round * 31 + small % 1009);
				for (auto &list : adds) {
					for (int i = 0; i < per_thread; ++i) {
						u64 amount = i % 5 == 4 ? (u64(1) << 32) : (u64(1) << 32) - 1 - rng() % 3;
						list.push_back(amount);
						total += amount;
					}
				}
				u64 bound = Seeded<C>::saturate(u128(seeded.initial()) + total);
				std::atomic<int> go{0}, running{threads};
				std::vector<std::thread> ts;
				for (int t = 0; t < threads; ++t) {
					ts.emplace_back([&, t] {
						while (!go.load(std::memory_order_acquire)) {
						}
						for (u64 amount : adds[t]) {
							c.add(amount);
						}
						running.fetch_sub(1);
					});
				}
				long fell = 0, above = 0;
				std::thread reader([&] {
					u64 previous = 0;
					while (running.load() > 0) {
						u64 v = c.get();
						fell += v < previous;
						above += v > bound;
						previous = v;
					}
				});
				go.store(1, std::memory_order_release);
				for (auto &t : ts) {
					t.join();
				}
				reader.join();
				CHECK(fell == 0 && above == 0);
				CHECK(c.get() == bound);
				++runs;
			}
		}
	}
	std::printf("%-40s %ld concurrent runs hold\n", name, runs);
}

// observe_max from near the top of the word (the compare-exchange counter takes it, and so does
// the large word of the contended one).
template <class C> void maxima(const char *name) {
	for (u64 seed : seeds) {
		Seeded<C> seeded(0, seed);
		auto &c = *seeded;
		u64 largest = seed;
		for (u64 v : amounts) {
			c.observe_max(v);
			largest = largest < v ? v : largest;
			CHECK(c.get() == largest);
		}
	}
	std::printf("%-40s observe_max holds\n", name);
}

int main() {
	sequential<Atomic64>("AtomicCounter from 64-bit seeds", false);
	sequential<Contended64>("ContendedCounter from 64-bit seeds", true);
	concurrent<Atomic64>("AtomicCounter, 6 threads", false);
	concurrent<Contended64>("ContendedCounter, 6 threads", true);
	// AtomicCounter's one word is the first; the contended counter's maxima live in its second.
	maxima<Contended64>("ContendedCounter");
	std::puts("metrics seeded tests passed");
}
