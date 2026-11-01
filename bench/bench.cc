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

// One duration observation on a histogram, one thread: ns per observe(), net of the loop that
// draws the values (the "disabled" row is that loop alone), best of five runs.
//
// The comparators are the histograms one writes by hand for the same buckets: a binary search
// over the 14 bounds (std::lower_bound) and plain increments, and the same with relaxed
// atomics (fetch_add for the counts and the sum, a compare-exchange loop for the maximum, which
// std::atomic has no operation for). Neither saturates. Every histogram sees the same values and
// its snapshot must equal the owner histogram's, bucket for bucket, count, sum and maximum.
#include "metrics.hh"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using kronuz::metrics::DurationSnapshot;
using kronuz::metrics::duration_bounds_ns;

class HandWrittenHistogram {
  public:
	void observe(std::uint64_t nanoseconds) noexcept {
		auto bucket = std::lower_bound(duration_bounds_ns.begin(), duration_bounds_ns.end(), nanoseconds) -
					  duration_bounds_ns.begin();
		++buckets_[static_cast<std::size_t>(bucket)];
		++count_;
		sum_ += nanoseconds;
		maximum_ = std::max(maximum_, nanoseconds);
	}
	DurationSnapshot snapshot() const noexcept {
		DurationSnapshot result;
		result.buckets = buckets_;
		result.count = count_;
		result.sum_ns = sum_;
		result.maximum_ns = maximum_;
		return result;
	}

  private:
	std::array<std::uint64_t, duration_bounds_ns.size() + 1> buckets_{};
	std::uint64_t count_ = 0, sum_ = 0, maximum_ = 0;
};

class HandWrittenAtomicHistogram {
  public:
	void observe(std::uint64_t nanoseconds) noexcept {
		auto bucket = std::lower_bound(duration_bounds_ns.begin(), duration_bounds_ns.end(), nanoseconds) -
					  duration_bounds_ns.begin();
		buckets_[static_cast<std::size_t>(bucket)].fetch_add(1, std::memory_order_relaxed);
		count_.fetch_add(1, std::memory_order_relaxed);
		sum_.fetch_add(nanoseconds, std::memory_order_relaxed);
		auto seen = maximum_.load(std::memory_order_relaxed);
		while (seen < nanoseconds &&
			   !maximum_.compare_exchange_weak(seen, nanoseconds, std::memory_order_relaxed)) {
		}
	}
	DurationSnapshot snapshot() const noexcept {
		DurationSnapshot result;
		for (std::size_t i = 0; i < result.buckets.size(); ++i) {
			result.buckets[i] = buckets_[i].load();
		}
		result.count = count_.load();
		result.sum_ns = sum_.load();
		result.maximum_ns = maximum_.load();
		return result;
	}

  private:
	std::array<std::atomic<std::uint64_t>, duration_bounds_ns.size() + 1> buckets_{};
	std::atomic<std::uint64_t> count_{0}, sum_{0}, maximum_{0};
};

constexpr std::uint64_t iterations = 1000000;

struct Result {
	double seconds;
	DurationSnapshot snapshot;
};

// The run draws the values with xorshift64; with `observe` false it draws and discards them.
template <class Histogram> Result run(bool observe) {
	Histogram histogram;
	std::uint64_t random = 12345, checksum = 0;
	auto start = Clock::now();
	for (std::uint64_t i = 0; i < iterations; ++i) {
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		auto value = random % 100000000;
		checksum += value;
		if (observe) {
			histogram.observe(value);
		}
	}
	auto wall = std::chrono::duration<double>(Clock::now() - start).count();
	volatile std::uint64_t keep = checksum;
	(void)keep;
	return {wall, histogram.snapshot()};
}

template <class Histogram> double best(bool observe, DurationSnapshot& snapshot) {
	double seconds = 1e18;
	for (int repetition = 0; repetition < 5; ++repetition) {
		auto result = run<Histogram>(observe);
		seconds = std::min(seconds, result.seconds);
		snapshot = result.snapshot;
	}
	return seconds;
}

bool same(const DurationSnapshot& a, const DurationSnapshot& b) {
	return a.buckets == b.buckets && a.count == b.count && a.sum_ns == b.sum_ns && a.maximum_ns == b.maximum_ns;
}
} // namespace

int main() {
	try {
		DurationSnapshot owner, snapshot, ignored;
		double loop = best<kronuz::metrics::DurationHistogram<>>(false, ignored);
		double owner_seconds = best<kronuz::metrics::DurationHistogram<>>(true, owner);
		if (owner.count != iterations) {
			throw std::runtime_error("owner histogram count mismatch");
		}
		std::printf("%-40s %6s %s\n", "histogram", "bytes", "ns per observe (net of the loop)");
		auto row = [&](const char* name, std::size_t bytes, double seconds, const DurationSnapshot& result) {
			std::printf("%-40s %6zu %6.2f%s\n", name, bytes, (seconds - loop) * 1e9 / iterations,
						same(result, owner) ? "" : "   MISMATCH");
			if (!same(result, owner)) {
				throw std::runtime_error("snapshot differs from the owner histogram");
			}
		};
		row("DurationHistogram<>", sizeof(kronuz::metrics::DurationHistogram<>), owner_seconds, owner);
		double seconds = best<HandWrittenHistogram>(true, snapshot);
		row("hand-written, lower_bound, plain", sizeof(HandWrittenHistogram), seconds, snapshot);
		seconds = best<kronuz::metrics::AtomicDurationHistogram>(true, snapshot);
		row("AtomicDurationHistogram", sizeof(kronuz::metrics::AtomicDurationHistogram), seconds, snapshot);
		seconds = best<kronuz::metrics::ContendedDurationHistogram>(true, snapshot);
		row("ContendedDurationHistogram", sizeof(kronuz::metrics::ContendedDurationHistogram), seconds, snapshot);
		seconds = best<HandWrittenAtomicHistogram>(true, snapshot);
		row("hand-written, lower_bound, atomic", sizeof(HandWrittenAtomicHistogram), seconds, snapshot);
		std::printf("(the loop alone: %.2f ns per value)\n", loop * 1e9 / iterations);
	} catch (const std::exception& error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
