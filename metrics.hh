/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace kronuz::metrics {

class Counter {
  public:
	void add(std::uint64_t amount = 1) noexcept { value_ += std::min(amount, maximum - value_); }
	void observe_max(std::uint64_t value) noexcept { value_ = std::max(value_, value); }
	std::uint64_t get() const noexcept { return value_; }

  private:
	static constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t value_ = 0;
};
// Saturating atomic counters, templated on the word and the atomic so test/model.cc can run the
// code at 8 bits against a model atomic. Use the AtomicCounter and ContendedCounter aliases.
//
// BasicAtomicCounter: one word updated by compare-and-swap, so add and observe_max are each atomic,
// the value never wraps, and a reader never sees it decrease. A fetch_add on this word could wrap.
template <class Word, template <class> class Atomic> class BasicAtomicCounter {
  public:
	void add(Word amount = 1) noexcept {
		auto current = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(current, Word(current + std::min(amount, Word(maximum - current))),
		                                     std::memory_order_relaxed)) {
		}
	}
	void observe_max(Word value) noexcept {
		auto current = value_.load(std::memory_order_relaxed);
		while (current < value && !value_.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
		}
	}
	Word get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	static constexpr Word maximum = std::numeric_limits<Word>::max();
	Atomic<Word> value_{0};
};
// BasicContendedCounter: the value is the saturating sum of two words. `small_` takes amounts
// below 2^(bits/2) with one fetch_add, after a load below 2^(bits-1); with n adds in flight it
// stays under 2^bits while n <= 2^(bits/2 - 1), so it cannot wrap. Every other add, and
// observe_max, is a compare-and-swap on `large_`. Both words only grow, so the sum is exact and a
// reader never sees it decrease. Use add or observe_max on a counter, not both.
template <class Word, template <class> class Atomic> class BasicContendedCounter {
  public:
	void add(Word amount = 1) noexcept {
		if (amount < fast_amount && small_.load(std::memory_order_relaxed) < fast_total) [[likely]] {
			small_.fetch_add(amount, std::memory_order_relaxed);
			return;
		}
		auto current = large_.load(std::memory_order_relaxed);
		while (!large_.compare_exchange_weak(current, Word(current + std::min(amount, Word(maximum - current))),
		                                     std::memory_order_relaxed)) {
		}
	}
	void observe_max(Word value) noexcept {
		auto current = large_.load(std::memory_order_relaxed);
		while (current < value && !large_.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
		}
	}
	Word get() const noexcept {
		Word small = small_.load(std::memory_order_relaxed), large = large_.load(std::memory_order_relaxed);
		return large > Word(maximum - small) ? maximum : Word(small + large);
	}

  private:
	static constexpr unsigned bits = std::numeric_limits<Word>::digits;
	static constexpr Word maximum = std::numeric_limits<Word>::max();
	static constexpr Word fast_total = Word(Word(1) << (bits - 1)), fast_amount = Word(Word(1) << (bits / 2));
	Atomic<Word> small_{0}, large_{0};
};
using AtomicCounter = BasicAtomicCounter<std::uint64_t, std::atomic>;
using ContendedCounter = BasicContendedCounter<std::uint64_t, std::atomic>;

// Inclusive nanosecond bounds; the final bucket is the overflow bucket.
inline constexpr std::array<std::uint64_t, 14> duration_bounds_ns{
	1000,	  4000,		16000,	   64000,	   256000,	   1000000,		4000000,
	16000000, 64000000, 256000000, 1000000000, 4000000000, 16000000000, 64000000000};
struct DurationSnapshot {
	std::array<std::uint64_t, duration_bounds_ns.size() + 1> buckets{};
	std::uint64_t count = 0, sum_ns = 0, maximum_ns = 0;
};
template <class CounterType = Counter> class DurationHistogram {
  public:
	void observe(std::uint64_t nanoseconds) noexcept {
		// The index of the first bound >= nanoseconds is the number of bounds below it
		// (the bounds are sorted); counting is branchless, std::lower_bound is not.
		std::size_t bucket = 0;
		for (auto bound : duration_bounds_ns) {
			bucket += bound < nanoseconds;
		}
		buckets_[bucket].add();
		count_.add();
		sum_.add(nanoseconds);
		maximum_.observe_max(nanoseconds);
	}
	DurationSnapshot snapshot() const noexcept {
		DurationSnapshot result;
		for (std::size_t i = 0; i < result.buckets.size(); ++i) {
			result.buckets[i] = buckets_[i].get();
		}
		result.count = count_.get();
		result.sum_ns = sum_.get();
		result.maximum_ns = maximum_.get();
		return result;
	}

  private:
	std::array<CounterType, duration_bounds_ns.size() + 1> buckets_{};
	CounterType count_, sum_, maximum_;
};
using AtomicDurationHistogram = DurationHistogram<AtomicCounter>;
using ContendedDurationHistogram = DurationHistogram<ContendedCounter>;

} // namespace kronuz::metrics
