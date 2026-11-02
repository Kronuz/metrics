# metrics

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/metrics/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/metrics/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only saturating counters and duration histograms for C++20: no allocation, and exact sums under contention.
Counters stop at 2^64 - 1 instead of wrapping, and a histogram files a
nanosecond duration into one of 15 buckets with a branchless count over 14
constant bounds. Reach for it for hot-path counters and latency histograms that
your server snapshots and exports itself.

## ✨ Highlights

- **Saturating, never wrapping.** `Counter::add(n)` stops at 2^64 - 1, and
  `observe_max(v)` keeps the largest value seen.
- **Atomic when shared.** `AtomicCounter` updates one word with a
  compare-exchange, so `add` and `observe_max` can be mixed across threads.
- **Built for contention.** `ContendedCounter` takes adds below 2^32 with one
  `fetch_add` that never retries, in 16 bytes, and stays exactly as saturating
  as `AtomicCounter`.
- **Duration histograms.** `DurationHistogram<>` files nanoseconds into 15
  buckets from 1 µs to 64 s (1, 4, 16, 64 and 256 of each unit, plus one for
  the rest) and keeps the count, the saturating sum and the maximum;
  `AtomicDurationHistogram` and `ContendedDurationHistogram` share it across
  threads.
- **Plain snapshots.** `snapshot()` copies the state into a `DurationSnapshot`
  struct.
- **Checked against a model.** `test/model.cc` explores every interleaving of
  two to five threads at 8 bits, and `tools/mutants.py` requires a test to fail
  on each of fifteen broken variants.

## 🚀 Quick start

```cpp
#include "metrics.hh"

#include <cstdint>
#include <iostream>

int main() {
	kronuz::metrics::DurationHistogram<> latency;
	latency.observe(12'000);       // 12 µs, in the bucket up to 16 µs
	latency.observe(3'000'000);    // 3 ms, in the bucket up to 4 ms

	auto s = latency.snapshot();
	std::cout << s.count << ' ' << s.sum_ns << ' ' << s.maximum_ns << ' '
	          << s.buckets[2] << ' ' << s.buckets[6] << '\n';   // 2 3012000 3000000 1 1

	kronuz::metrics::Counter requests;
	requests.add(UINT64_MAX);
	requests.add();                // saturates instead of wrapping
	std::cout << (requests.get() == UINT64_MAX) << '\n';        // 1
}
```

## 📦 Installation

Requires a C++20 compiler and CMake 3.16 or newer; no dependencies. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan,
and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(metrics
  GIT_REPOSITORY https://github.com/Kronuz/metrics.git
  GIT_TAG        41a72134892eef4f1eecf20f149dc0b84cea9650)  # or main for the tip
FetchContent_MakeAvailable(metrics)

target_link_libraries(my_app PRIVATE metrics::metrics)
```

```cpp
#include "metrics.hh"
```

## 🎯 When to use it

Use it for hot-path counters and latency histograms with fixed memory, updated
by one thread or many.

Not for:

- **Exporting.** Snapshots are plain structs; formatting for Prometheus or
  similar is yours.
- **Mixing `add` and `observe_max` on a `ContendedCounter`.** Its
  `observe_max` maximizes only one of its two words; use one or the other.
- **A `ContendedCounter` without contention.** It is twice the size of an
  `AtomicCounter` and slower at two threads on arm64 (see below).
- **Percentiles finer than a bucket.** The bounds are fixed, a factor of about
  4 apart.

## ⚡ Performance

[`metrics_counter_bench`](bench/counter_bench.cc) has every thread add 1 to one counter
(2,000,000 adds per thread) and reports ns per add per thread, so a higher number at more
threads is contention. `AtomicCounter` is one word updated by a compare-exchange loop;
`ContendedCounter` is two words, where an add below 2^32 is one `fetch_add` that never
retries. Both saturate at 2^64 - 1. The comparators are the two obvious hand-written counters:
a saturating compare-exchange loop, and a bare relaxed `std::atomic<std::uint64_t>::fetch_add`,
which wraps instead of saturating. Every counter must end at the number of adds made, and the
program prints `MISMATCH` when one does not. Cells are the better of two runs of best-of-5;
"× faster" is `AtomicCounter`'s time over `ContendedCounter`'s, so a value below 1 is a loss.

Apple M4 Pro (macOS 26), Apple clang 17.0.0 with `-O3 -DNDEBUG`:

| Threads | `AtomicCounter` | `ContendedCounter` | × faster | CAS loop | bare `fetch_add` |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.1 | 1.8 | 1.17 | 2.0 | 1.8 |
| 2 | 5.0 | 7.9 | 0.63 | 4.9 | 6.7 |
| 4 | 58.3 | 35.4 | 1.65 | 60.0 | 31.1 |
| 8 | 289 | 130 | 2.22 | 247 | 145 |

Intel Core i9-9980HK (macOS 15, x86-64), Apple clang 17.0.0 with `-O3 -DNDEBUG`:

| Threads | `AtomicCounter` | `ContendedCounter` | × faster | CAS loop | bare `fetch_add` |
|---:|---:|---:|---:|---:|---:|
| 1 | 7.8 | 7.6 | 1.03 | 7.7 | 4.4 |
| 2 | 63.4 | 41.6 | 1.52 | 67.4 | 23.9 |
| 4 | 210 | 86.7 | 2.42 | 214 | 67.7 |
| 8 | 521 | 181 | 2.88 | 536 | 130 |

`ContendedCounter` is ahead of `AtomicCounter` from four threads on both machines and from two
on the i9, and behind it at two threads on the M4 Pro; `AtomicCounter` and the hand-written
compare-exchange loop are level. Saturation costs against the bare `fetch_add`: up to 1.2 times
on the M4 Pro (`ContendedCounter` is 10% ahead of it at 8 threads) and 1.3 to 1.7 times on the
i9. The 8-thread rows vary by up to 60% between runs.

[`metrics_bench`](bench/bench.cc) times one `observe` of a duration on a histogram, one thread, in
ns, over 1,048,576 values drawn beforehand (best of five runs, the better of two). The comparators are
the histograms one writes by hand for the same 15 buckets: `std::lower_bound` over the bounds and
plain increments, and the same with relaxed atomics (`fetch_add` for the counts and the sum, a
compare-exchange loop for the maximum); neither saturates. Every histogram sees the same values, and
the benchmark fails when a snapshot differs from `DurationHistogram<>`'s.

| Histogram | Bytes | M4 Pro | i9 |
|---|---:|---:|---:|
| `DurationHistogram<>` | 144 | 2.5 | 3.2 |
| hand-written, `lower_bound`, plain | 144 | 2.2 | 4.8 |
| `AtomicDurationHistogram` | 144 | 2.8 | 15.7 |
| `ContendedDurationHistogram` | 288 | 2.3 | 14.7 |
| hand-written, `lower_bound`, atomic | 144 | 2.7 | 18.5 |

On the M4 Pro the plain hand-written histogram takes 12% less time than `DurationHistogram<>`, which
saturates; on the i9 the library is 1.5 times ahead of it. `ContendedDurationHistogram` is ahead of the
hand-written atomic histogram on both machines (1.2 times on the M4 Pro, 1.3 on the i9), and
`AtomicDurationHistogram` is level with it on the M4 Pro and 1.2 times ahead on the i9.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMETRICS_BUILD_BENCH=ON
cmake --build build
./build/metrics_counter_bench && ./build/metrics_bench
```

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): the counters, why the contended counter
  splits its value over two words, the histogram, and how they are verified.
- [CHANGELOG.md](CHANGELOG.md): what changed.
- [AGENTS.md](AGENTS.md): repository map and invariants.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#metrics): the catalog entry.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Add `-DMETRICS_BUILD_BENCH=ON` to build `metrics_bench` and
`metrics_counter_bench`. `python3 tools/mutants.py` runs the mutation check.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
