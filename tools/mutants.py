#!/usr/bin/env python3
"""Break metrics.hh in many ways and require the tests to notice.

Each mutant edits metrics.hh in a scratch copy of the repository, builds the test programs from
that copy and runs them. A mutant is killed when a test program fails or times out. The script
exits non-zero if any mutant survives, stops building, or no longer matches its source text, so a
test that stops pinning a rule of the counters shows up here.

The mutants include the two ways the proof of the counters has been broken: a single-word fetch_add
fast path (the wrap that is permanent once another thread's add lifts the word to the maximum) and a
ContendedCounter whose `small_` word is not checked before the fetch_add.

usage: tools/mutants.py [--cxx c++] [--jobs N] [--filter SUBSTRING] [--all]
  --all  run every test program on a mutant instead of stopping at the first one that fails
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = "\t"

# (id, old text, new text, what it breaks[, number of matches])
MUTANTS = [
    ("counter-unclamped", "value_ += std::min(amount, maximum - value_);", "value_ += amount;",
     "Counter::add wraps instead of saturating"),
    ("counter-max-overwrites", "value_ = std::max(value_, value);", "value_ = value;",
     "Counter::observe_max keeps the last value, not the largest"),
    ("atomic-fetch-add-fast-path",
     T * 2 + "while (!value_.compare_exchange_weak(current, Word(current + std::min(amount, Word(maximum - current))),",
     T * 2 + "if (current < Word(Word(1) << (std::numeric_limits<Word>::digits - 1)) &&\n" +
     T * 3 + "amount < Word(Word(1) << (std::numeric_limits<Word>::digits / 2))) {\n" +
     T * 3 + "value_.fetch_add(amount, std::memory_order_relaxed);\n" + T * 3 + "return;\n" + T * 2 + "}\n" +
     T * 2 + "while (!value_.compare_exchange_weak(current, Word(current + std::min(amount, Word(maximum - current))),",
     "AtomicCounter::add takes a fetch_add after a load below 2^63: a wrap in flight is permanent (CA2)"),
    ("add-unclamped", "std::min(amount, Word(maximum - current))", "amount",
     "AtomicCounter::add and ContendedCounter::add wrap instead of saturating", 2),
    ("atomic-observe-max-stored",
     T * 2 + "while (current < value && !value_.compare_exchange_weak(current, value, std::memory_order_relaxed)) {\n" +
     T * 2 + "}",
     T * 2 + "if (current < value) {\n" + T * 3 + "value_.store(value, std::memory_order_relaxed);\n" + T * 2 + "}",
     "AtomicCounter::observe_max loads, tests and stores: a concurrent larger maximum is lost"),
    ("contended-no-small-check", "amount < fast_amount && small_.load(std::memory_order_relaxed) < fast_total",
     "amount < fast_amount", "ContendedCounter::add does not look at `small_` before the fetch_add"),
    ("contended-total-at-maximum", "fast_total = Word(Word(1) << (bits - 1))", "fast_total = maximum",
     "`small_` takes fetch_adds until it is at the maximum, so one more wraps it"),
    ("contended-fast-amount-doubled", "fast_amount = Word(Word(1) << (bits / 2))",
     "fast_amount = Word(Word(1) << (bits / 2 + 1))",
     "`small_` takes amounts one bit wider: five adds of 31 in flight at 8 bits cross 2^bits (model only)"),
    ("contended-wide-amounts", "fast_amount = Word(Word(1) << (bits / 2))", "fast_amount = Word(Word(1) << (bits - 1))",
     "`small_` takes amounts up to 2^(bits-1): a few adds in flight cross 2^bits"),
    ("contended-get-unclamped", "return large > Word(maximum - small) ? maximum : Word(small + large);",
     "return Word(small + large);", "ContendedCounter::get adds the two words without saturating"),
    ("histogram-bound-exclusive", "bucket += bound < nanoseconds;", "bucket += bound <= nanoseconds;",
     "a value equal to a bound lands in the next bucket"),
    ("histogram-last-bound", "16000000000, 64000000000};", "16000000000, 64000000001};",
     "the last bucket bound moves by a nanosecond"),
    ("histogram-no-maximum", "maximum_.observe_max(nanoseconds);", "", "the histogram does not track its maximum"),
    ("histogram-sum", "sum_.add(nanoseconds);", "sum_.add(1);", "the histogram adds one to its sum, not the value"),
    ("histogram-no-count", "count_.add();", "", "the histogram does not count observations"),
]

# program name -> sources, flags, label of the CTest test it is
PROGRAMS = [
    ("seeded", ["test/seeded.cc"], [], "metrics_seeded"),
    ("test", ["test/test.cc"], [], "metrics"),
    ("differential", ["test/differential.cc"], [], "metrics_differential"),
    ("model", ["test/model.cc"], [], "metrics_model"),
]
TIMEOUT = 600


def run(cmd, timeout=None):
    """Returns (exit status, output); a timeout is status 124."""
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        return 124, str(e.stdout or "")


def trial(cxx, work, everything):
    """Build and run the programs in `work`. Returns (labels of the programs that failed, build error)."""
    failed = []
    for name, sources, flags, label in PROGRAMS:
        exe = os.path.join(work, "bin-" + name)
        cmd = [cxx, "-std=c++20", "-O2", "-I" + work, "-o", exe] + flags + [os.path.join(work, s) for s in sources] + ["-pthread"]
        status, out = run(cmd)
        if status != 0:
            if "static assertion failed" in out:  # a test that pins a value at compile time
                failed.append(label + " (static_assert)")
                if not everything:
                    break
                continue
            return None, out
        status, out = run([exe], TIMEOUT)
        if status != 0:
            failed.append(label)
            if not everything:
                break
    return failed, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--filter", default="")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="metrics-mutants-") as tmp:
        def fresh(name):
            work = os.path.join(tmp, name)
            shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".git", "build*", "*.o"))
            return work

        failed, err = trial(args.cxx, fresh("clean"), True)
        if failed is None or failed:
            print("the unmodified sources do not pass: " + ("build failed\n" + err if failed is None else " ".join(failed)))
            return 2
        print("clean: all tests pass", flush=True)

        def one(m):
            mid, old, new, what = m[:4]
            matches = m[4] if len(m) > 4 else 1
            work = fresh(mid)
            path = os.path.join(work, "metrics.hh")
            text = open(path).read()
            if text.count(old) != matches:
                return mid, "STALE", "%d matches of %r, expected %d" % (text.count(old), old, matches)
            open(path, "w").write(text.replace(old, new))
            failed, err = trial(args.cxx, work, args.all)
            shutil.rmtree(work)
            if failed is None:
                lines = [l for l in err.splitlines() if "error" in l]
                return mid, "BROKEN", lines[0] if lines else err[-200:]
            if failed:
                return mid, "killed", "by %s (%s)" % (", ".join(failed), what)
            return mid, "SURVIVES", what

        todo = [m for m in MUTANTS if args.filter in m[0]]
        bad = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for mid, status, detail in pool.map(one, todo):
                print("%-30s %-8s %s" % (mid, status, detail), flush=True)
                if status != "killed":
                    bad.append(mid)
        print("%d mutants, %d killed" % (len(todo), len(todo) - len(bad)))
        if bad:
            print("not killed: " + " ".join(bad))
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
