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

ns per `add(1)` per thread, every thread adding to one counter
(`metrics_counter_bench`, `-O3`), best of 5, the range over two runs:

| | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---|---|---|---|
| `AtomicCounter`, Apple M4 Pro | 2.1 | 5.2 to 5.3 | 67.9 to 68.7 | 337 to 342 |
| `ContendedCounter`, Apple M4 Pro | 1.9 | 6.8 to 7.1 | 34.1 to 42.7 | 194 to 200 |
| `AtomicCounter`, Intel Core i9-9980HK | 7.8 to 8.2 | 67.1 to 76.4 | 214 to 230 | 530 to 626 |
| `ContendedCounter`, Intel Core i9-9980HK | 7.5 to 7.8 | 43.1 to 50.1 | 88.3 to 89.0 | 180 to 192 |

A bare `fetch_add` with no saturation costs 7.0 ns at two threads and 190 ns at
eight on the M4 Pro, and 28 and 140 ns on the i9. One `observe` on a histogram,
one thread (`metrics_bench`): `DurationHistogram<>` takes 2.4 to 2.7 ns on the
M4 Pro and 6.0 to 6.8 ns on the i9, `AtomicDurationHistogram` 2.9 to 3.2 and
17.8 to 18.9, and `ContendedDurationHistogram` 2.8 to 3.1 and 17.3 to 18.1
(288 bytes instead of 144).

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
