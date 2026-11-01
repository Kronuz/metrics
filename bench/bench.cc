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

// One duration observation on a histogram, one thread: ns per observe(), best of five runs of
// 1,048,576 observations (16 passes over 65,536 values drawn beforehand, so the loop only reads a value
// and observes it).
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

constexpr std::size_t passes = 16;

std::vector<std::uint64_t> draw() {
	std::vector<std::uint64_t> values(65536);
	std::uint64_t random = 12345;
	for (auto& value : values) {
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		value = random % 100000000;
	}
	return values;
}

// Seconds for the fastest of five runs on a fresh histogram; the snapshot is the last run's.
template <class Histogram> double best(const std::vector<std::uint64_t>& values, DurationSnapshot& snapshot) {
	double seconds = 1e18;
	for (int repetition = 0; repetition < 5; ++repetition) {
		Histogram histogram;
		auto start = Clock::now();
		for (std::size_t pass = 0; pass < passes; ++pass) {
			for (auto value : values) {
				histogram.observe(value);
			}
		}
		seconds = std::min(seconds, std::chrono::duration<double>(Clock::now() - start).count());
		snapshot = histogram.snapshot();
	}
	return seconds;
}

bool same(const DurationSnapshot& a, const DurationSnapshot& b) {
	return a.buckets == b.buckets && a.count == b.count && a.sum_ns == b.sum_ns && a.maximum_ns == b.maximum_ns;
}
} // namespace

int main() {
	try {
		const auto values = draw();
		const double observations = double(values.size() * passes);
		DurationSnapshot owner, snapshot;
		double owner_seconds = best<kronuz::metrics::DurationHistogram<>>(values, owner);
		if (owner.count != values.size() * passes) {
			throw std::runtime_error("owner histogram count mismatch");
		}
		std::printf("%-40s %6s %s\n", "histogram", "bytes", "ns per observe");
		auto row = [&](const char* name, std::size_t bytes, double seconds, const DurationSnapshot& result) {
			std::printf("%-40s %6zu %6.2f%s\n", name, bytes, seconds * 1e9 / observations,
						same(result, owner) ? "" : "   MISMATCH");
			if (!same(result, owner)) {
				throw std::runtime_error("snapshot differs from the owner histogram");
			}
		};
		row("DurationHistogram<>", sizeof(kronuz::metrics::DurationHistogram<>), owner_seconds, owner);
		double seconds = best<HandWrittenHistogram>(values, snapshot);
		row("hand-written, lower_bound, plain", sizeof(HandWrittenHistogram), seconds, snapshot);
		seconds = best<kronuz::metrics::AtomicDurationHistogram>(values, snapshot);
		row("AtomicDurationHistogram", sizeof(kronuz::metrics::AtomicDurationHistogram), seconds, snapshot);
		seconds = best<kronuz::metrics::ContendedDurationHistogram>(values, snapshot);
		row("ContendedDurationHistogram", sizeof(kronuz::metrics::ContendedDurationHistogram), seconds, snapshot);
		seconds = best<HandWrittenAtomicHistogram>(values, snapshot);
		row("hand-written, lower_bound, atomic", sizeof(HandWrittenAtomicHistogram), seconds, snapshot);
	} catch (const std::exception& error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
