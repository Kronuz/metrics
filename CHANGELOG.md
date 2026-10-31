# Changelog

All notable changes to `metrics`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-10-31

### Changed

- `AtomicCounter::add` takes one `fetch_add` while the counter is below 2^63
  and the amount below 2^32, and a clamping CAS loop otherwise. The previous
  add was a compare-exchange retry loop that spun under contention
  (`aa7a3d8`, `9207dd3`).
- The histogram bucket index is the count of bounds below the value, a
  branchless loop, instead of `std::lower_bound` (`aa7a3d8`).

### Fixed

- Saturation is exact again. The first `fetch_add` version (`aa7a3d8`) added
  unconditionally and stored the maximum on a cold path when the sum wrapped,
  so a concurrent reader could see the wrapped value for an instant after one
  huge add pinned the counter (review finding CR2). `9207dd3` limits the
  `fetch_add` to sums that cannot wrap.

### Added

- `test/differential.cc`, a differential test against the frozen CAS counter
  and `std::lower_bound` (edges, random, concurrent saturation), plus
  saturation-visibility tests.
- `metrics_counter_bench`, a contended-counter benchmark.

### Performance

Apple arm64, ns per add per thread, shared host, best of 5:

| threads | before (CAS loop) | after | bare `fetch_add` |
|---|---|---|---|
| 4 | 65.7 | 24.5 | 23.7 |
| 8 | 404 | 154 | 165 |

1 and 2 threads were unchanged.
