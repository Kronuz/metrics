# AGENTS.md — metrics

## Repo map

```
metrics.hh        Counter, BasicAtomicCounter (AtomicCounter), BasicContendedCounter (ContendedCounter),
                  DurationHistogram, DurationSnapshot.
test/test.cc      Saturation, maxima, bucket bounds, concurrent atomic updates.
test/differential.cc  Both atomic counters, at 64 and 16 bits, against a frozen CAS counter (sequences, totals,
                  pinned values, monotonic readers, small adds racing a pin); buckets against std::lower_bound.
test/model.cc     Every interleaving of both atomic counters at 8 bits, and the broken counters it must reject.
bench/bench.cc    Update cost of each counter and histogram (metrics_bench).
bench/counter_bench.cc  Contended add (metrics_counter_bench, not built by default).
CHANGELOG.md      Dated changes.
docs/metrics.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/metrics.d2 docs/metrics.svg
```

## Invariants

- Counters saturate at 2^64 - 1; they never wrap, and no reader sees a value
  below an earlier one.
- `AtomicCounter` updates one word with a compare-exchange loop, so `add` and
  `observe_max` are each atomic and may be mixed. No `fetch_add` fast path can be
  added to a single word: a `fetch_add` in flight can wrap after another thread's
  add lifts the same word to the maximum, and the wrap is permanent
  (`test/model.cc` finds it in four steps at 8 bits).
- `ContendedCounter` keeps two words and returns their saturating sum. `small_`
  only ever receives `fetch_add` of amounts below 2^(bits/2), and only after a
  load below 2^(bits-1). After `small_` first reaches 2^(bits-1) each thread adds
  to it at most once more (its next add loads its own result), so with n adds in
  flight `small_` is at most 2^(bits-1) - 1 + n * (2^(bits/2) - 1), which fits in
  `bits` bits while n <= 2^(bits/2 - 1): 2^31 at 64 bits, 8 at 8 bits. `large_`
  takes every other add with a clamping compare-exchange. Both words only grow and
  neither wraps, so the sum is exact. Keep `small_` to those `fetch_add`s alone;
  moving an amount between the words, or `observe_max` on `small_`, breaks the
  bound.
- `ContendedCounter::observe_max` maximizes `large_` only, so use a
  `ContendedCounter` with `add` or with `observe_max`, not both.
- The atomic counters use relaxed operations. The arguments above need only the
  coherence of each word, which gives each reader a nondecreasing value.
- Bucket bounds are inclusive and fixed; changing them changes every consumer's
  exported histograms.
- No allocation and no locks on any update path.
- `test/model.cc` runs the counter code itself against a model atomic (every
  atomic call is one step), at 8 bits, from every initial state around the
  thresholds. It is sequentially consistent and has no spurious
  compare-exchange failures; `test/differential.cc` covers the relaxed
  operations under ThreadSanitizer. Keep the broken counters in `model.cc`
  rejected: each one pins a way the proof fails.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

After changing a counter, also break it once (for example, drop the `small_`
check in `ContendedCounter::add`, or put a `fetch_add` fast path back in
`AtomicCounter::add`) and confirm `metrics_model` and `metrics_differential` fail.
