# Changelog

All notable changes to `metrics`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-10-31

### Fixed

- `AtomicCounter::add` wraps no more (review findings CA2 and CR2). `aa7a3d8` replaced
  the compare-exchange loop with an unconditional `fetch_add` and a store of the
  maximum after a wrap, so a reader could see the wrapped value for an instant.
  `9207dd3` took the `fetch_add` only after loading a value below 2^63, for an amount
  below 2^32, and called saturation exact. It was not: a thread preempted between the
  load and the `fetch_add` adds after another thread's large add has lifted the same
  word to `UINT64_MAX`, the sum wraps, and the wrap is permanent. Four threads adding 1
  two hundred times while one adds `UINT64_MAX` ended below the maximum in 15618 of
  20000 trials on arm64, 4753 of 5000 on x86_64 (macOS) and 72 of 5000 on Linux x86_64
  (gcc 11). No `fetch_add` fast path is sound on a single word, so `AtomicCounter` is
  the compare-exchange loop again, with the same machine code as before `aa7a3d8` on
  arm64 and x86_64. `add` and `observe_max` are each atomic and can be mixed.

### Added

- `ContendedCounter` and `ContendedDurationHistogram`: the contended-add speedup,
  where it can be proved. The value is the saturating sum of two words. `small_`
  takes amounts below 2^32 with one `fetch_add`, after a load below 2^63; `large_`
  takes every other add, and `observe_max`, with a clamping compare-exchange. With
  `n` adds in flight `small_` stays below 2^64 while `n <= 2^31`, so neither word
  wraps and the value is exactly the saturating sum. It is 16 bytes, and use it with
  `add` or with `observe_max`, not both.
- `BasicAtomicCounter<Word, Atomic>` and `BasicContendedCounter<Word, Atomic>`;
  `AtomicCounter` and `ContendedCounter` are the 64-bit `std::atomic` instances, so
  the tests run the real code at 8 and 16 bits.
- `test/model.cc` (`metrics_model`): every interleaving of two to four threads of adds,
  `observe_max` and `get()` at 8 bits, from initial states around the thresholds, with
  the counter code running against a model atomic. It checks that the value never
  decreases, that it lies between the saturated totals of the finished and of the
  started operations, that each `get()` lies between its first and last step, and
  that the final value is exact. Seven broken counters must be rejected, among them
  the single-word `fetch_add` fast path (four steps) and the `aa7a3d8` version.
  Eight adds in flight hold the bound of `ContendedCounter` and nine break it.
- `test/differential.cc`: both counters at 64 and 16 bits against a frozen
  compare-exchange counter (sequences through the thresholds, totals, pinned values,
  monotonic readers) and the CA2 race with a concurrent reader; the bucket index
  against `std::lower_bound` at every edge and on random values.
- `metrics_counter_bench`, a contended-counter benchmark, times `ContendedCounter`
  and `metrics_bench` times `ContendedDurationHistogram`.

### Changed

- The histogram bucket index is the count of bounds below the value, a branchless
  loop, instead of `std::lower_bound` (`aa7a3d8`).

### Performance

ns per `add(1)` per thread, every thread adding to one counter, best of 5, two runs each
(`metrics_counter_bench`, `-O3`; each host held under `quiet strict`). `AtomicCounter` is the
compare-exchange loop of the start, `AtomicCounter9207dd3` its `fetch_add` fast path as of
`9207dd3`, which compiles to the same hot path as `ContendedCounter::add(1)`.

| threads | M4 Pro `AtomicCounter` | M4 Pro `ContendedCounter` | M4 Pro `AtomicCounter9207dd3` | i9 `AtomicCounter` | i9 `ContendedCounter` | i9 `AtomicCounter9207dd3` |
|---|---|---|---|---|---|---|
| 1 | 2.1, 2.1 | 1.9, 1.9 | 2.0 | 7.8, 8.2 | 7.5, 7.8 | 7.5 |
| 2 | 5.3, 5.2 | 6.8, 7.1 | 7.8 | 76.4, 67.1 | 50.1, 43.1 | 41.7 |
| 4 | 68.7, 67.9 | 42.7, 34.1 | 42.9 | 230, 214 | 89.0, 88.3 | 88.6 |
| 8 | 342, 337 | 200, 194 | 178 | 626, 530 | 192, 180 | 187 |

`ContendedCounter` is 1.7-1.8x faster than `AtomicCounter` at 4 and 8 threads on the M4 Pro
and 2.5-3.1x on the i9, and 1.5x faster at 2 threads on the i9. At two threads on the M4 Pro
it is about 30% slower (a bare `fetch_add` costs 7.0 ns there), so `AtomicCounter` is the
counter for little contention. `AtomicCounter` and the start's loop agree within the noise at
every thread count, and `AtomicCounter::add` has the same instructions as at the start on
arm64 and x86_64.

`observe` on a histogram, one thread, ns per observation (`metrics_bench`, three readings per
run, two runs): owner `DurationHistogram<>` 2.4-2.7 on the M4 Pro (3.3-3.5 at the start) and
6.0-6.8 on the i9 (7.2-8.0); `AtomicDurationHistogram` 2.9-3.2 (3.8-4.0) and 17.8-18.9
(18.3-21.1); `ContendedDurationHistogram` 2.8-3.1 and 17.3-18.1, at 288 bytes instead of 144.
The owner histogram compiles to the same instructions as at `9207dd3`.

This corrects earlier entries of this date: the speedup on arm64 is 1.7-2.0x, not 2.7x (65.7
to 24.5 ns at 4 threads, 404 to 154 ns at 8 threads, measured on a busy host), and one
and two threads were not unchanged.
