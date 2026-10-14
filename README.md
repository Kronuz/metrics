# metrics

Bounded counters and duration histograms for C++20, header-only, with no dependencies.

- `kronuz::metrics::Counter`: a single-owner `std::uint64_t` counter. `add(n)` saturates at
  the maximum instead of wrapping; `observe_max(v)` keeps the largest value seen.
- `kronuz::metrics::AtomicCounter`: the same with relaxed atomics, for counters several
  threads update.
- `kronuz::metrics::DurationHistogram<Counter>`: nanosecond durations in 15 buckets with
  inclusive bounds from 1 µs to 64 s, growing by 4× (the last bucket holds everything
  above 64 s), plus the count, the saturating sum, and the maximum.
  `AtomicDurationHistogram` uses `AtomicCounter`. `snapshot()` copies the state out as a
  plain `DurationSnapshot`.

Nothing allocates, and an update is a few instructions: the bucket is a binary search
over 14 constant bounds.

## Usage

```cpp
#include "metrics.hh"

kronuz::metrics::DurationHistogram<> latency;
latency.observe(12'000);                 // 12 µs
auto s = latency.snapshot();             // s.buckets, s.count, s.sum_ns, s.maximum_ns
```

With CMake:

```cmake
FetchContent_Declare(metrics GIT_REPOSITORY https://github.com/Kronuz/metrics.git GIT_TAG <commit>)
FetchContent_MakeAvailable(metrics)
target_link_libraries(app PRIVATE metrics::metrics)
```

## Build and test

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
./build/metrics_bench
```

## License

MIT. See `LICENSE`.
