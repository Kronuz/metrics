# Architecture

`metrics.hh` holds three counters and one histogram template. Nothing allocates, and every
counter saturates at 2^64 - 1 instead of wrapping. [AGENTS.md](AGENTS.md) states the
invariants as rules for changing the code; this document explains them.

## The counters

- `Counter` is a single-owner `std::uint64_t`. `add(n)` adds `min(n, maximum - value)`, and
  `observe_max(v)` keeps the largest value seen.
- `AtomicCounter` is the same with relaxed atomics. Every update is one compare-exchange on
  one word, so `add` and `observe_max` are each atomic and can be mixed.
- `ContendedCounter` is for counters many threads `add` to. It keeps two words (16 bytes),
  and amounts below 2^32 take one `fetch_add`, so an add never retries.

Both atomic counters are templates on the word and the atomic (`BasicAtomicCounter<Word,
Atomic>`, `BasicContendedCounter<Word, Atomic>`); `AtomicCounter` and `ContendedCounter` are
the 64-bit `std::atomic` instances. `test/model.cc` runs the same code at 8 bits over a model
atomic.

## Contended counters

Under contention the compare-exchange loop of `AtomicCounter` retries, and the retries cost
more than the add. A `fetch_add` never retries, but a bare `fetch_add` cannot saturate: if it
runs after another thread has lifted the same word to `UINT64_MAX`, the sum wraps. So no
`fetch_add` is added to a single word.

`ContendedCounter` splits the value over two words and returns their saturating sum:

- `small_` takes every add below 2^32 with one `fetch_add`, but only after loading a value
  below 2^63. Once it first reaches 2^63 each thread adds to it at most once more, so with
  `n` adds in flight it stays below 2^64 while `n <= 2^31`.
- `large_` takes every other add, and `observe_max`, with a clamping compare-exchange.

Each word only grows and neither wraps, so the value is exactly the saturating sum, a
reader never sees it decrease, and once it reads `UINT64_MAX` it reads `UINT64_MAX` forever.

It is 16 bytes and costs more than `AtomicCounter` where there is no contention to remove (two
threads on arm64; see the README's Performance section). Use it with `add` or with
`observe_max`, not both: `observe_max` maximizes `large_` only.

## The histogram

`DurationHistogram<CounterType>` keeps 15 bucket counters, a count, a saturating sum and a
maximum, all of `CounterType`. The 14 inclusive bounds in `duration_bounds_ns` run from 1 µs
to 64 s (1, 4, 16, 64 and 256 of each unit), and the last bucket holds everything above 64 s.
The bucket index is the count of bounds below the value, a branchless loop over the constant
bounds. `snapshot()` copies the state into a plain `DurationSnapshot`.
`AtomicDurationHistogram` and `ContendedDurationHistogram` are the instances over the atomic
counters.

## Verification

`test/model.cc` runs the counter code itself, at 8 bits, against a model atomic in which every
load, store, `fetch_add` and compare-exchange is one step. It explores every interleaving of two
to four threads of adds, `observe_max` and `get()` from initial states around the thresholds, and
checks that the value never decreases, that it stays between the saturated totals of the finished
and the started operations, that each `get()` returns a value from between its first and last
step, and that the final value is exact. It includes several adds per thread from nonzero
states and every interleaving of four and five threads adding 15 at `small_` = 127, and of five
adding 31, below the eight-thread bound, besides the schedule that crosses the bound. It also
rejects seven broken counters, among them a single-word `fetch_add` fast path.

`test/differential.cc` checks both counters at 64 and 16 bits against a frozen compare-exchange
counter, including small adds racing a large one with a reader, and runs under
ThreadSanitizer. `test/seeded.cc` builds both counters at 64 bits over an atomic that records
its words, stores starting values just below, at and above 2^63 and near 2^64 into them, and
checks every add and every concurrent run against a 128-bit oracle: the 64-bit `small_` check
cannot be reached by real adds. `tools/mutants.py` breaks `metrics.hh` in fifteen ways (the
single-word `fetch_add`, the `small_` check, the clamps, the bucket bounds) and requires a test
to fail on each.
