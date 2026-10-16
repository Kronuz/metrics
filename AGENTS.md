# AGENTS.md — metrics

## Repo map

```
metrics.hh        Counter, AtomicCounter, DurationHistogram, DurationSnapshot.
test/test.cc      Saturation, maxima, bucket bounds, concurrent atomic updates.
bench/bench.cc    Update cost of each counter and histogram.
docs/metrics.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/metrics.d2 docs/metrics.svg
```

## Invariants

- Counters saturate at 2^64 - 1; they never wrap.
- Bucket bounds are inclusive and fixed; changing them changes every consumer's
  exported histograms.
- No allocation and no locks on any update path.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
