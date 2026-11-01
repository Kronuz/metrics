// Exhaustive interleaving check of the atomic counters, at 8 bits.
//
// The counters are templates on the word and on the atomic. Here the word is std::uint8_t and the
// atomic is a model whose every load, store, fetch_add and compare-exchange is one scheduling
// step, so the production add/observe_max/get code runs unchanged against it. A thread is a
// script of operations; the checker explores every interleaving of the steps of two to four
// threads (memoized on the shared words and each thread's progress), from each initial state
// given, and checks at every reachable state and edge that
//   - the logical value (the counter's own get()) never decreases,
//   - it lies between the saturated total of the operations completed so far and of those started,
//   - every get() returns a value between the value at its first and its last step,
//   - when all threads finish the value is exactly the saturated total.
// The previous single-word fast path and other broken counters are checked too and must be rejected.
//
// How a step is executed: the model has no threads. To advance thread t it re-runs the thread's
// current operation from its start; the first k atomic calls return the results logged for them,
// call k+1 executes for real and is logged, and the remaining calls of that run return dummy
// results (a load gives 0, a compare-exchange succeeds) without touching the shared words. This
// relies on each operation being a deterministic function of the results of its atomic calls.
// Limits: sequentially consistent steps (the counters use relaxed operations, and their proof
// uses only per-word coherence, which test/differential.cc exercises under ThreadSanitizer), and
// a compare-exchange never fails spuriously (a spurious failure re-runs the loop on the freshly
// read value, the same as a step that found the word changed).
#include "metrics.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace kronuz::metrics;
using W = std::uint8_t;

namespace {

struct Entry {
	W value;
	bool ok;
};
struct Context {
	std::array<W, 4> cells{};
	int cell_count = 0;
	const std::vector<Entry> *replay = nullptr;
	Entry fresh{0, true};
	std::size_t index = 0;
	bool snapshot = false, stepped = false;
};
Context *context = nullptr;

template <class T> class ModelAtomic {
  public:
	ModelAtomic(T initial) : cell_(context->cell_count++) { context->cells[cell_] = initial; }
	T load(std::memory_order = std::memory_order_seq_cst) const {
		return step([](W &cell) { return Entry{cell, true}; }).value;
	}
	void store(T value, std::memory_order = std::memory_order_seq_cst) {
		step([&](W &cell) {
			cell = value;
			return Entry{value, true};
		});
	}
	T fetch_add(T amount, std::memory_order = std::memory_order_seq_cst) {
		return step([&](W &cell) {
			W old = cell;
			cell = W(old + amount);
			return Entry{old, true};
		}).value;
	}
	bool compare_exchange_weak(T &expected, T desired, std::memory_order = std::memory_order_seq_cst) {
		auto entry = step([&](W &cell) {
			if (cell == expected) {
				cell = desired;
				return Entry{desired, true};
			}
			return Entry{cell, false};
		});
		if (!entry.ok) {
			expected = entry.value;
		}
		return entry.ok;
	}

  private:
	template <class F> Entry step(F real) const {
		auto &c = *context;
		if (c.snapshot) {
			return real(c.cells[cell_]);
		}
		auto i = c.index++;
		if (i < c.replay->size()) {
			return (*c.replay)[i];
		}
		if (c.stepped) {
			return Entry{0, true};
		}
		c.stepped = true;
		return c.fresh = real(c.cells[cell_]);
	}
	int cell_;
};

// Frozen and broken counters the checker must reject (the first is the previous head).
template <class Word, template <class> class Atomic> class SingleWordFastPath {
  public:
	void add(Word amount = 1) noexcept {
		auto current = value_.load(std::memory_order_relaxed);
		if (current < Word(128) && amount < Word(16)) {
			value_.fetch_add(amount, std::memory_order_relaxed);
			return;
		}
		while (!value_.compare_exchange_weak(current, Word(amount > Word(255 - current) ? 255 : current + amount),
		                                     std::memory_order_relaxed)) {
		}
	}
	void observe_max(Word) noexcept {}
	Word get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	Atomic<Word> value_{0};
};
template <class Word, template <class> class Atomic> class FetchAddThenFixUp {
  public:
	void add(Word amount = 1) noexcept {
		auto previous = value_.fetch_add(amount, std::memory_order_relaxed);
		if (previous > Word(255 - amount)) {
			value_.store(255, std::memory_order_relaxed);
		}
	}
	void observe_max(Word) noexcept {}
	Word get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	Atomic<Word> value_{0};
};
template <class Word, template <class> class Atomic> class CasWithoutClamp {
  public:
	void add(Word amount = 1) noexcept {
		auto current = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(current, Word(current + amount), std::memory_order_relaxed)) {
		}
	}
	void observe_max(Word) noexcept {}
	Word get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	Atomic<Word> value_{0};
};
// BasicContendedCounter with one rule broken: Skip = no check of `small_` before the fetch_add,
// Wide = fast amounts up to 127 (so a few adds in flight can cross 2^8), Unsaturated = get() adds
// the words without clamping.
template <bool Skip, bool Wide, bool Unsaturated = false> struct BrokenContended {
	template <class Word, template <class> class Atomic> class Counter {
	  public:
		void add(Word amount = 1) noexcept {
			if (amount < Word(Wide ? 128 : 16) && (Skip || small_.load(std::memory_order_relaxed) < Word(128))) {
				small_.fetch_add(amount, std::memory_order_relaxed);
				return;
			}
			auto current = large_.load(std::memory_order_relaxed);
			while (!large_.compare_exchange_weak(current, Word(current + std::min(amount, Word(255 - current))),
			                                     std::memory_order_relaxed)) {
			}
		}
		void observe_max(Word) noexcept {}
		Word get() const noexcept {
			Word small = small_.load(std::memory_order_relaxed), large = large_.load(std::memory_order_relaxed);
			return !Unsaturated && large > Word(255 - small) ? 255 : Word(small + large);
		}

	  private:
		Atomic<Word> small_{0}, large_{0};
	};
};
// BasicAtomicCounter with observe_max as a load, a test and a store (a concurrent maximum can be lost).
template <class Word, template <class> class Atomic> class CasWithStoredMax {
  public:
	void add(Word amount = 1) noexcept {
		auto current = value_.load(std::memory_order_relaxed);
		while (!value_.compare_exchange_weak(current, Word(current + std::min(amount, Word(255 - current))),
		                                     std::memory_order_relaxed)) {
		}
	}
	void observe_max(Word value) noexcept {
		if (value_.load(std::memory_order_relaxed) < value) {
			value_.store(value, std::memory_order_relaxed);
		}
	}
	Word get() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
	Atomic<Word> value_{0};
};

enum class Kind : char { add = 'a', max = 'm', get = 'g' };
struct Op {
	Kind kind;
	W amount;
};
using Script = std::vector<Op>;
// mode: 's' every update is an add (value = saturating sum), 'x' every update is an observe_max
// (value = maximum), 'm' mixed (the counter must be linearizable; checked at the end).
struct Scenario {
	std::array<W, 4> init{};
	std::vector<Script> threads;
	char mode = 's';
};
struct Progress {
	std::size_t op = 0;
	std::vector<Entry> log;
	int value_at_start = 0;
};
struct State {
	std::array<W, 4> cells{};
	std::vector<Progress> threads;
};

struct Totals {
	std::uint64_t scenarios = 0, states = 0, edges = 0;
};

template <class C> class Checker {
  public:
	// The initial value is computed here, not by the counter under test: the words add up, saturating.
	explicit Checker(const Scenario &scenario)
	    : sc_(scenario), initial_(std::min(255, int(scenario.init[0]) + int(scenario.init[1]))) {}

	// Returns an empty string when every interleaving satisfies the invariants.
	std::string run(Totals &totals) {
		State state;
		state.cells = sc_.init;
		state.threads.resize(sc_.threads.size());
		++totals.scenarios;
		if (value(state.cells) != initial_) {
			failure_ = "get() of the initial state is " + std::to_string(value(state.cells)) + ", not " +
			           std::to_string(initial_);
			return failure_;
		}
		dfs(state);
		totals.states += states_;
		totals.edges += edges_;
		if (!failure_.empty()) {
			failure_ += " (schedule of thread ids:";
			for (auto t : path_) {
				failure_ += ' ' + std::to_string(t);
			}
			failure_ += ')';
		}
		return failure_;
	}

	// One schedule instead of all: runs the threads in the order of `schedule` (one step each),
	// then finishes them lowest id first.
	std::string run_schedule(const std::vector<int> &schedule) {
		State state;
		state.cells = sc_.init;
		state.threads.resize(sc_.threads.size());
		for (std::size_t step = 0;; ++step) {
			int t = -1;
			if (step < schedule.size()) {
				t = schedule[step];
			} else {
				for (std::size_t i = 0; i < sc_.threads.size() && t < 0; ++i) {
					t = state.threads[i].op < sc_.threads[i].size() ? int(i) : -1;
				}
			}
			if (t < 0) {
				return check_final(state) ? std::string() : failure_;
			}
			bool done;
			int result = 0;
			State next = advance(state, t, done, result);
			int before = value(state.cells), after = value(next.cells);
			if (after < before) {
				return "value decreased from " + std::to_string(before) + " to " + std::to_string(after);
			}
			if (!check_bounds(next, after)) {
				return failure_;
			}
			state = std::move(next);
		}
	}

  private:
	static int value(const std::array<W, 4> &cells) {
		Context c;
		c.snapshot = true;
		context = &c;
		int result;
		{
			C counter;
			c.cells = cells;
			result = counter.get();
		}
		context = nullptr;
		return result;
	}
	static int combine(char mode, int acc, int amount) {
		return mode == 'x' ? std::max(acc, amount) : std::min(255, acc + amount);
	}
	static std::string key(const State &s) {
		std::string k(s.cells.begin(), s.cells.end());
		for (auto &t : s.threads) {
			k += char(t.op);
			k += char(t.log.size());
			for (auto e : t.log) {
				k += char(e.value);
				k += char(e.ok);
			}
			k += char(t.value_at_start);
			k += '|';
		}
		return k;
	}
	bool fail(std::string why) {
		failure_ = std::move(why);
		return false;
	}
	// Executes the next step of thread t; `done` tells whether its operation completed instead.
	State advance(const State &s, int t, bool &done, int &result) const {
		const Op &op = sc_.threads[t][s.threads[t].op];
		Context c;
		c.replay = &s.threads[t].log;
		context = &c;
		W returned = 0;
		{
			C counter;
			c.cells = s.cells;
			switch (op.kind) {
				case Kind::add: counter.add(op.amount); break;
				case Kind::max: counter.observe_max(op.amount); break;
				case Kind::get: returned = counter.get(); break;
			}
		}
		context = nullptr;
		State next = s;
		next.cells = c.cells;
		done = !c.stepped;
		if (done) {
			++next.threads[t].op;
			next.threads[t].log.clear();
			result = returned;
		} else {
			if (op.kind == Kind::get && s.threads[t].log.empty()) {
				next.threads[t].value_at_start = value(s.cells);
			}
			next.threads[t].log.push_back(c.fresh);
		}
		return next;
	}
	bool check_bounds(const State &s, int v) {
		if (sc_.mode == 'm') {
			return true;
		}
		int lo = initial_, hi = initial_;
		for (std::size_t t = 0; t < sc_.threads.size(); ++t) {
			for (std::size_t j = 0; j <= s.threads[t].op && j < sc_.threads[t].size(); ++j) {
				const Op &op = sc_.threads[t][j];
				if (op.kind == Kind::get) {
					continue;
				}
				if (j < s.threads[t].op) {
					lo = combine(sc_.mode, lo, op.amount);
					hi = combine(sc_.mode, hi, op.amount);
				} else if (!s.threads[t].log.empty()) {
					hi = combine(sc_.mode, hi, op.amount);
				}
			}
		}
		if (v < lo) {
			return fail("value " + std::to_string(v) + " below the completed total " + std::to_string(lo));
		}
		if (v > hi) {
			return fail("value " + std::to_string(v) + " above the started total " + std::to_string(hi));
		}
		return true;
	}
	bool check_final(const State &s) {
		int v = value(s.cells);
		if (sc_.mode == 'm') {
			std::vector<Op> ops;
			for (auto &script : sc_.threads) {
				ops.insert(ops.end(), script.begin(), script.end());
			}
			std::sort(ops.begin(), ops.end(), [](const Op &a, const Op &b) {
				return std::pair(a.kind, a.amount) < std::pair(b.kind, b.amount);
			});
			do {
				int acc = initial_;
				for (auto &op : ops) {
					acc = op.kind == Kind::add ? combine('s', acc, op.amount) : combine('x', acc, op.amount);
				}
				if (acc == v) {
					return true;
				}
			} while (std::next_permutation(ops.begin(), ops.end(), [](const Op &a, const Op &b) {
				return std::pair(a.kind, a.amount) < std::pair(b.kind, b.amount);
			}));
			return fail("final value " + std::to_string(v) + " matches no sequential order");
		}
		int total = initial_;
		for (auto &script : sc_.threads) {
			for (auto &op : script) {
				if (op.kind != Kind::get) {
					total = combine(sc_.mode, total, op.amount);
				}
			}
		}
		if (v != total) {
			return fail("final value " + std::to_string(v) + " is not the saturated total " + std::to_string(total));
		}
		return true;
	}
	bool dfs(const State &s) {
		if (!seen_.insert(key(s)).second) {
			return true;
		}
		++states_;
		int v = value(s.cells);
		bool any = false;
		for (std::size_t t = 0; t < sc_.threads.size(); ++t) {
			if (s.threads[t].op >= sc_.threads[t].size()) {
				continue;
			}
			any = true;
			bool done;
			int result = 0;
			State next = advance(s, int(t), done, result);
			++edges_;
			path_.push_back(int(t));
			int nv = value(next.cells);
			if (nv < v) {
				return fail("value decreased from " + std::to_string(v) + " to " + std::to_string(nv));
			}
			if (!check_bounds(next, nv)) {
				return false;
			}
			if (done && sc_.threads[t][s.threads[t].op].kind == Kind::get) {
				if (result < s.threads[t].value_at_start || result > nv) {
					return fail("get returned " + std::to_string(result) + ", outside [" +
					            std::to_string(s.threads[t].value_at_start) + ", " + std::to_string(nv) + "]");
				}
			}
			if (!dfs(next)) {
				return false;
			}
			path_.pop_back();
		}
		return any || check_final(s);
	}

	const Scenario &sc_;
	int initial_;
	std::unordered_set<std::string> seen_;
	std::vector<int> path_;
	std::string failure_;
	std::uint64_t states_ = 0, edges_ = 0;
};

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::abort();                                                        \
		}                                                                        \
	} while (0)

using Cas = BasicAtomicCounter<W, ModelAtomic>;
using Contended = BasicContendedCounter<W, ModelAtomic>;

const std::vector<W> amounts_small{1, 15, 16, 127, 128, 200, 255};
const std::vector<W> amounts_edge{0, 1, 2, 15, 16, 17, 127, 128, 129, 254, 255};

std::string describe(const Scenario &sc) {
	std::string text = "init [";
	for (int i = 0; i < 2; ++i) {
		text += std::to_string(sc.init[i]) + (i ? "" : ",");
	}
	text += "]";
	for (std::size_t t = 0; t < sc.threads.size(); ++t) {
		text += " T" + std::to_string(t) + ":";
		for (auto &op : sc.threads[t]) {
			text += std::string(" ") + char(op.kind) + (op.kind == Kind::get ? "" : std::to_string(op.amount));
		}
	}
	return text;
}

// Runs every scenario; a counter that must hold fails the test on the first violation.
template <class C> void expect_holds(const char *name, const std::vector<Scenario> &scenarios) {
	Totals totals;
	for (auto &sc : scenarios) {
		auto failure = Checker<C>(sc).run(totals);
		if (!failure.empty()) {
			std::fprintf(stderr, "FAIL %s: %s\n  %s\n", name, describe(sc).c_str(), failure.c_str());
			std::abort();
		}
	}
	std::printf("%-44s %6llu scenarios %10llu states %10llu edges  all interleavings hold\n", name,
	            (unsigned long long)totals.scenarios, (unsigned long long)totals.states,
	            (unsigned long long)totals.edges);
}
// A broken counter must be rejected by at least one scenario.
template <class C> void expect_rejected(const char *name, const std::vector<Scenario> &scenarios) {
	Totals totals;
	for (auto &sc : scenarios) {
		auto failure = Checker<C>(sc).run(totals);
		if (!failure.empty()) {
			std::printf("%-44s rejected: %s\n  %s\n", name, describe(sc).c_str(), failure.c_str());
			return;
		}
	}
	std::fprintf(stderr, "FAIL %s: the checker found no violation in %llu scenarios\n", name,
	             (unsigned long long)totals.scenarios);
	std::abort();
}

Scenario add_scenario(std::array<W, 4> init, std::vector<std::vector<W>> adds, char mode = 's') {
	Scenario sc;
	sc.init = init;
	sc.mode = mode;
	for (auto &list : adds) {
		Script script;
		for (auto amount : list) {
			script.push_back({mode == 'x' ? Kind::max : Kind::add, amount});
		}
		sc.threads.push_back(script);
	}
	return sc;
}

// Every assignment of `values` to `slots` adds spread over `threads` scripts of equal length,
// dropping assignments that only permute whole threads.
std::vector<Scenario> grid(std::array<W, 4> init, int threads, int per_thread, const std::vector<W> &values,
                           char mode = 's', bool reader = false) {
	std::vector<Scenario> out;
	int slots = threads * per_thread;
	std::vector<int> index(slots, 0);
	for (;;) {
		std::vector<std::vector<W>> adds(threads);
		for (int i = 0; i < slots; ++i) {
			adds[i / per_thread].push_back(values[index[i]]);
		}
		if (std::is_sorted(adds.begin(), adds.end())) {
			auto sc = add_scenario(init, adds, mode);
			if (reader) {
				sc.threads.push_back({{Kind::get, 0}, {Kind::get, 0}});
			}
			out.push_back(std::move(sc));
		}
		int i = 0;
		while (i < slots && ++index[i] == int(values.size())) {
			index[i++] = 0;
		}
		if (i == slots) {
			return out;
		}
	}
}
void append(std::vector<Scenario> &to, std::vector<Scenario> from) {
	to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	// Sanitizer and unoptimized builds run the same model with the four-thread families dropped.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(__OPTIMIZE__)
	constexpr bool full = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	constexpr bool full = false;
#else
	constexpr bool full = true;
#endif
#else
	constexpr bool full = true;
#endif
	if (!full) {
		std::puts("reduced model (sanitizer or unoptimized build): no four-thread scenarios");
	}
	static_assert(sizeof(AtomicCounter) == 8 && sizeof(ContendedCounter) == 16);

	// Counts from zero: two to four threads of adds, and a reader taking two gets.
	std::vector<Scenario> from_zero;
	append(from_zero, grid({}, 2, 2, amounts_small));
	append(from_zero, grid({}, 3, 1, amounts_edge));
	if (full) {
		append(from_zero, grid({}, 4, 1, amounts_small));
		append(from_zero, grid({}, 3, 2, {1, 16, 255}));
	}
	append(from_zero, grid({}, 2, 1, amounts_small, 's', true));
	append(from_zero, grid({}, 2, 2, {1, 16, 255}, 's', true));
	append(from_zero, grid({}, 3, 1, {1, 16, 255}, 's', true));
	expect_holds<Cas>("AtomicCounter from zero", from_zero);
	expect_holds<Contended>("ContendedCounter from zero", from_zero);

	// Any initial state: every combination of the two words around their thresholds.
	std::vector<Scenario> any_state_cas, any_state_contended;
	const std::vector<W> small_words = full ? std::vector<W>{0, 1, 112, 127, 128, 129, 200, 254, 255}
	                                        : std::vector<W>{0, 127, 128, 255};
	const std::vector<W> large_words = full ? std::vector<W>{0, 1, 100, 128, 254, 255} : std::vector<W>{0, 128, 255};
	for (W s : small_words) {
		append(any_state_cas, grid({s}, 2, 1, amounts_edge));
		append(any_state_cas, grid({s}, 3, 1, {1, 16, 128, 255}));
		if (full) {
			append(any_state_cas, grid({s}, 4, 1, {1, 16, 255}));
		}
		for (W l : large_words) {
			append(any_state_contended, grid({s, l}, 2, 1, amounts_edge));
			append(any_state_contended, grid({s, l}, 3, 1, {1, 15, 16, 255}));
			if (full) {
				append(any_state_contended, grid({s, l}, 4, 1, {1, 15, 255}));
			}
			append(any_state_contended, grid({s, l}, 2, 1, {1, 15, 255}, 's', true));
		}
	}
	expect_holds<Cas>("AtomicCounter from any state", any_state_cas);
	expect_holds<Contended>("ContendedCounter from any state", any_state_contended);

	// observe_max alone, and add with observe_max on the compare-exchange counter.
	std::vector<Scenario> maxima, maxima_contended;
	append(maxima, grid({}, 3, 1, amounts_edge, 'x'));
	append(maxima, grid({}, 2, 2, {1, 16, 128, 255}, 'x', true));
	append(maxima_contended, grid({0, 0}, 3, 1, amounts_edge, 'x'));
	append(maxima_contended, grid({0, 50}, 2, 2, {1, 16, 128, 255}, 'x', true));
	expect_holds<Cas>("AtomicCounter observe_max", maxima);
	expect_holds<Contended>("ContendedCounter observe_max", maxima_contended);
	std::vector<Scenario> mixed;
	for (W a : {1, 100, 200}) {
		for (W m : {50, 150, 255}) {
			for (W start : {0, 120}) {
				Scenario sc;
				sc.mode = 'm';
				sc.init = {start};
				sc.threads = {{{Kind::add, a}, {Kind::max, m}}, {{Kind::max, W(m / 2)}, {Kind::add, a}}};
				mixed.push_back(sc);
			}
		}
	}
	expect_holds<Cas>("AtomicCounter add mixed with observe_max", mixed);

	// The number of adds in flight that ContendedCounter tolerates is 2^(8/2 - 1) = 8. The worst
	// schedule, every thread loading `small_` (127) before any adds, leaves 127 + 8 * 15 = 247 in
	// `small_` for 8 threads and wraps it for 9. Only this schedule is run, not all of them.
	for (int threads : {8, 9}) {
		auto sc = add_scenario({127}, std::vector<std::vector<W>>(threads, {15}));
		std::vector<int> every_thread_loads_first;
		for (int t = 0; t < threads; ++t) {
			every_thread_loads_first.push_back(t);
		}
		auto failure = Checker<Contended>(sc).run_schedule(every_thread_loads_first);
		std::printf("%-44s %s\n", ("ContendedCounter " + std::to_string(threads) + " adds in flight").c_str(),
		            failure.empty() ? "holds" : ("wraps: " + failure).c_str());
		CHECK(failure.empty() == (threads == 8));
	}

	// The checker must reject the broken counters.
	std::vector<Scenario> probes = grid({}, 2, 1, {1, 255});
	append(probes, grid({250, 0}, 2, 1, {7, 15}));
	append(probes, grid({127, 0}, 2, 1, {100}));
	expect_rejected<BrokenContended<true, false>::Counter<W, ModelAtomic>>("mutant: fetch_add without the small_ check",
	                                                                      probes);
	expect_rejected<BrokenContended<false, true>::Counter<W, ModelAtomic>>("mutant: fast amounts up to 2^7", probes);
	expect_rejected<BrokenContended<false, false, true>::Counter<W, ModelAtomic>>("mutant: get() without clamping",
	                                                                             grid({200, 100}, 1, 1, {1}));
	expect_rejected<SingleWordFastPath<W, ModelAtomic>>("mutant: single-word fetch_add fast path (CA2)", probes);
	expect_rejected<FetchAddThenFixUp<W, ModelAtomic>>("mutant: fetch_add then store the maximum (CR2)", probes);
	expect_rejected<CasWithoutClamp<W, ModelAtomic>>("mutant: compare-exchange without clamping", probes);
	expect_rejected<CasWithStoredMax<W, ModelAtomic>>("mutant: observe_max as load, test and store",
	                                                  grid({}, 2, 1, {3, 5}, 'x'));

	std::puts("metrics model tests passed");
}
