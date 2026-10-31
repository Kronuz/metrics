# AGENTS.md — metrics

## Repo map

```
metrics.hh        Counter, AtomicCounter, DurationHistogram, DurationSnapshot.
test/test.cc      Saturation, maxima, bucket bounds, concurrent atomic updates.
test/differential.cc  Differential test against a frozen CAS counter and std::lower_bound.
bench/bench.cc    Update cost of each counter and histogram (metrics_bench).
bench/counter_bench.cc  Contended AtomicCounter::add (metrics_counter_bench, not built by default).
CHANGELOG.md      Dated changes.
docs/metrics.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/metrics.d2 docs/metrics.svg
```

## Invariants

- Counters saturate at 2^64 - 1; they never wrap, and no reader sees a value
  below an earlier one.
- `AtomicCounter::add` takes `fetch_add` only when the loaded value is below
  2^63 and the amount below 2^32. Each thread has at most one such add in
  flight, so with fewer than 2^31 threads the value stays below
  2^63 + threads × 2^32 < 2^64. Every other add takes the clamping CAS loop.
- That bound assumes a counter is used either with `add` or with
  `observe_max`, not both concurrently: an `observe_max` that lifts the value
  above 2^63 between the load and the `fetch_add` can let that add wrap.
- Bucket bounds are inclusive and fixed; changing them changes every consumer's
  exported histograms.
- No allocation and no locks on any update path.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
