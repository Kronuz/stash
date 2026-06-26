// Stash contention benchmark.
//
// Measures the producer-side insert cost of Xapiand's `stash` timer wheel
// against the realistic alternatives, under the two access patterns that
// actually occur in Xapiand:
//
//   CONVERGENT  every producer inserts at key = now()  (the pathological
//               "immediate" pattern; note Xapiand routes immediate logs
//               INLINE, not through stash, so this is a stress test, not a
//               real path).
//   SPREAD      every producer inserts at key = now() + uniform(0, W)
//               (the real deferred-log / debounce / timer pattern).
//
// Structures:
//   stash      the real stash.h wheel, Xapiand's exact 4-level config
//   heap       std::priority_queue<(key,task)> behind one mutex   (scheduler)
//   multimap   std::multimap<key,task> behind one mutex           (scheduler)
//   deque      std::deque<task> behind one mutex     (FIFO hand-off only)
//   vyukov     lock-free intrusive MPSC queue        (FIFO hand-off only)
//   sharded    per-producer deque, lock per shard    (FIFO hand-off only)
//
// One consumer thread drains concurrently in every run, so the numbers
// include realistic add/walk contention on the shared atomics, not an
// insert microbench in isolation.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <map>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <random>
#include <functional>

#include "stash.h"

static constexpr unsigned long long MS = 1000000ULL;

static inline unsigned long long now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The value stashes/queues carry: a shared_ptr, exactly like the logger's
// shared_ptr<Logging>. The atomic refcount churn is part of the real cost and
// is paid identically by every structure.
struct Task {
	uint64_t id;
	std::atomic<bool> active;
	explicit Task(uint64_t i = 0) : id(i), active(true) {}
	explicit operator bool() const { return active.load(std::memory_order_relaxed); }
};
using TaskPtr = std::shared_ptr<Task>;

// ----------------------------------------------------------------------------
// stash: the real wheel, Xapiand's exact configuration from scheduler.h.
// ----------------------------------------------------------------------------
struct StashWheel {
	using Tasks = StashValues<TaskPtr, 10ULL, &now_ns>;
	using L1 = StashSlots<Tasks, 10ULL, &now_ns, 1ULL * MS, 50ULL, false>;
	using L2 = StashSlots<L1, 10ULL, &now_ns, 50ULL * MS, 10ULL, false>;
	using L3 = StashSlots<L2, 12ULL, &now_ns, 500ULL * MS, 36ULL, false>;
	using Wheel = StashSlots<L3, 4800ULL, &now_ns, 18000ULL * MS, 4800ULL, true>;

	StashContext ctx;
	StashContext cctx;
	Wheel queue;
	StashWheel() : ctx(now_ns()), cctx(now_ns()) {}

	static constexpr bool is_scheduler = true;
	void add(int /*tid*/, unsigned long long key, const TaskPtr& t) {
		try { queue.add(ctx, key, t); } catch (const std::out_of_range&) {}
	}
	uint64_t walk() {
		ctx.op = StashContext::Operation::walk;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = now_ns();
		uint64_t n = 0;
		TaskPtr out;
		while (queue.next(ctx, &out)) { if (out) ++n; out.reset(); }
		return n;
	}
};

// ----------------------------------------------------------------------------
// heap: std::priority_queue behind one mutex. The textbook scheduler.
// ----------------------------------------------------------------------------
struct MutexHeap {
	struct Item { unsigned long long key; uint64_t seq; TaskPtr t; };
	struct Cmp { bool operator()(const Item& a, const Item& b) const {
		return a.key != b.key ? a.key > b.key : a.seq > b.seq; } };
	std::mutex m;
	std::priority_queue<Item, std::vector<Item>, Cmp> pq;
	std::atomic<uint64_t> seqgen{0};

	static constexpr bool is_scheduler = true;
	void add(int, unsigned long long key, const TaskPtr& t) {
		uint64_t s = seqgen.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lk(m);
		pq.push(Item{key, s, t});
	}
	uint64_t walk() {
		auto cutoff = now_ns();
		uint64_t n = 0;
		for (;;) {
			TaskPtr t;
			{ std::lock_guard<std::mutex> lk(m);
			  if (pq.empty() || pq.top().key > cutoff) break;
			  t = pq.top().t; pq.pop(); }
			if (t) ++n;
		}
		return n;
	}
};

// ----------------------------------------------------------------------------
// multimap: std::multimap<key,task> behind one mutex.
// ----------------------------------------------------------------------------
struct MutexMultimap {
	std::mutex m;
	std::multimap<unsigned long long, TaskPtr> mm;

	static constexpr bool is_scheduler = true;
	void add(int, unsigned long long key, const TaskPtr& t) {
		std::lock_guard<std::mutex> lk(m);
		mm.emplace(key, t);
	}
	uint64_t walk() {
		auto cutoff = now_ns();
		uint64_t n = 0;
		for (;;) {
			TaskPtr t;
			{ std::lock_guard<std::mutex> lk(m);
			  if (mm.empty() || mm.begin()->first > cutoff) break;
			  t = mm.begin()->second; mm.erase(mm.begin()); }
			if (t) ++n;
		}
		return n;
	}
};

// ----------------------------------------------------------------------------
// deque: single mutex + std::deque. FIFO hand-off (no time ordering).
// ----------------------------------------------------------------------------
struct MutexDeque {
	std::mutex m;
	std::deque<TaskPtr> q;
	static constexpr bool is_scheduler = false;
	void add(int, unsigned long long, const TaskPtr& t) {
		std::lock_guard<std::mutex> lk(m);
		q.push_back(t);
	}
	uint64_t walk() {
		uint64_t n = 0;
		for (;;) {
			TaskPtr t;
			{ std::lock_guard<std::mutex> lk(m);
			  if (q.empty()) break;
			  t = q.front(); q.pop_front(); }
			if (t) ++n;
		}
		return n;
	}
};

// ----------------------------------------------------------------------------
// vyukov: canonical lock-free intrusive MPSC queue. FIFO hand-off.
// Producers funnel through ONE atomic exchange on `head` — the same class of
// single-hot-atomic contention as stash's leaf atom_end, just exchange not add.
// ----------------------------------------------------------------------------
struct VyukovMpsc {
	struct Node { std::atomic<Node*> next; TaskPtr t; };
	std::atomic<Node*> head;
	Node* tail;
	Node stub;
	VyukovMpsc() { stub.next.store(nullptr); head.store(&stub); tail = &stub; }
	static constexpr bool is_scheduler = false;

	void add(int, unsigned long long, const TaskPtr& t) {
		Node* n = new Node();
		n->t = t;
		n->next.store(nullptr, std::memory_order_relaxed);
		Node* prev = head.exchange(n, std::memory_order_acq_rel);
		prev->next.store(n, std::memory_order_release);
	}
	// single consumer
	bool pop(TaskPtr& out) {
		Node* t = tail;
		Node* next = t->next.load(std::memory_order_acquire);
		if (t == &stub) {
			if (!next) return false;
			tail = next; t = next;
			next = t->next.load(std::memory_order_acquire);
		}
		if (next) {
			tail = next;
			out = std::move(t->t);
			delete t;
			return true;
		}
		Node* h = head.load(std::memory_order_acquire);
		if (t != h) return false; // producer mid-push
		stub.next.store(nullptr, std::memory_order_relaxed);
		Node* prev = head.exchange(&stub, std::memory_order_acq_rel);
		prev->next.store(&stub, std::memory_order_release);
		next = t->next.load(std::memory_order_acquire);
		if (next) {
			tail = next;
			out = std::move(t->t);
			delete t;
			return true;
		}
		return false;
	}
	uint64_t walk() {
		uint64_t n = 0;
		TaskPtr out;
		while (pop(out)) { if (out) ++n; out.reset(); }
		return n;
	}
};

// ----------------------------------------------------------------------------
// sharded: one deque+mutex per producer thread. No shared hot point on the
// producer side; this is the moodycamel-style answer (per-producer subqueues).
// ----------------------------------------------------------------------------
struct ShardedQueue {
	struct alignas(64) Shard { std::mutex m; std::deque<TaskPtr> q; };
	std::vector<Shard> shards;
	explicit ShardedQueue(int n) : shards(n) {}
	static constexpr bool is_scheduler = false;
	void add(int tid, unsigned long long, const TaskPtr& t) {
		Shard& s = shards[tid];
		std::lock_guard<std::mutex> lk(s.m);
		s.q.push_back(t);
	}
	uint64_t walk() {
		uint64_t n = 0;
		for (auto& s : shards) {
			for (;;) {
				TaskPtr t;
				{ std::lock_guard<std::mutex> lk(s.m);
				  if (s.q.empty()) break;
				  t = s.q.front(); s.q.pop_front(); }
				if (t) ++n;
			}
		}
		return n;
	}
};

// ----------------------------------------------------------------------------
// Driver
// ----------------------------------------------------------------------------
struct Result {
	double mops;       // million inserts / sec (producer wall time)
	double p50_ns;
	double p99_ns;
	double p999_ns;
};

enum class Spread { Convergent, Window };

template <typename Q>
Result run_one(Q& q, int nthreads, uint64_t total, Spread spread, unsigned long long window_ns) {
	uint64_t per = total / nthreads;
	uint64_t real_total = per * nthreads;

	std::atomic<uint64_t> produced{0};
	std::atomic<bool> done{false};
	std::atomic<int> ready{0};
	std::atomic<bool> go{false};

	std::vector<std::vector<uint32_t>> lat(nthreads);
	for (auto& v : lat) v.reserve(per);

	auto producer = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> d(0, window_ns ? window_ns - 1 : 0);
		ready.fetch_add(1, std::memory_order_release);
		while (!go.load(std::memory_order_acquire)) {}
		for (uint64_t i = 0; i < per; ++i) {
			auto t = std::make_shared<Task>((uint64_t)tid << 40 | i);
			unsigned long long key = now_ns();
			if (spread == Spread::Window) key += d(rng);
			auto t0 = std::chrono::steady_clock::now();
			q.add(tid, key, t);
			auto t1 = std::chrono::steady_clock::now();
			lat[tid].push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		}
		produced.fetch_add(per, std::memory_order_release);
	};

	// One consumer, draining concurrently the whole time.
	auto consumer = [&]() {
		while (!go.load(std::memory_order_acquire)) {}
		while (!done.load(std::memory_order_acquire)) {
			q.walk();
		}
		q.walk();
	};

	std::vector<std::thread> prod;
	std::thread cons(consumer);
	for (int i = 0; i < nthreads; ++i) prod.emplace_back(producer, i);
	while (ready.load(std::memory_order_acquire) < nthreads) {}

	auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);
	for (auto& th : prod) th.join();
	auto end = std::chrono::steady_clock::now();
	done.store(true, std::memory_order_release);
	cons.join();

	double secs = std::chrono::duration<double>(end - start).count();

	std::vector<uint32_t> all;
	all.reserve(real_total);
	for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
	std::sort(all.begin(), all.end());
	auto pct = [&](double p) -> double {
		if (all.empty()) return 0;
		size_t idx = (size_t)(p * (all.size() - 1));
		return all[idx];
	};

	Result r;
	r.mops = (double)real_total / secs / 1e6;
	r.p50_ns = pct(0.50);
	r.p99_ns = pct(0.99);
	r.p999_ns = pct(0.999);
	return r;
}

template <typename Make>
void bench_row(const char* name, bool scheduler_only, Make make,
               const std::vector<int>& threads, uint64_t total, Spread spread, unsigned long long window_ns) {
	printf("  %-10s", name);
	for (int nt : threads) {
		Result best{0, 0, 0, 0};
		for (int rep = 0; rep < 3; ++rep) {
			auto q = make(nt);
			Result r = run_one(*q, nt, total, spread, window_ns);
			if (r.mops > best.mops) best = r; // best of 3 (least noise)
		}
		printf(" | %6.2f", best.mops);
	}
	printf("\n");
	// second line: p99 latency
	printf("  %-10s", "");
	for (int nt : threads) {
		Result best{0, 0, 0, 0};
		double bestp99 = 1e18;
		for (int rep = 0; rep < 3; ++rep) {
			auto q = make(nt);
			Result r = run_one(*q, nt, total, spread, window_ns);
			if (r.p99_ns < bestp99) { bestp99 = r.p99_ns; best = r; }
		}
		printf(" | %5.0fns", best.p99_ns);
	}
	printf("   (p99)\n");
	(void)scheduler_only;
}

int main(int argc, char** argv) {
	uint64_t total = 2000000;
	if (argc > 1) total = strtoull(argv[1], nullptr, 10);
	std::vector<int> threads = {1, 2, 4, 8, 14};

	printf("stash structures benchmark — total inserts = %llu, 1 consumer, best-of-3\n",
	       (unsigned long long)total);
	printf("Throughput = million inserts/sec (higher better); p99 = producer insert latency\n\n");

	auto stash_make = [](int) { return std::make_unique<StashWheel>(); };
	auto heap_make = [](int) { return std::make_unique<MutexHeap>(); };
	auto mm_make = [](int) { return std::make_unique<MutexMultimap>(); };
	auto deque_make = [](int) { return std::make_unique<MutexDeque>(); };
	auto vyukov_make = [](int) { return std::make_unique<VyukovMpsc>(); };
	auto sharded_make = [](int n) { return std::make_unique<ShardedQueue>(n); };

	auto header = [&](const char* title) {
		printf("%s\n  %-10s", title, "threads");
		for (int nt : threads) printf(" | %6d", nt);
		printf("\n");
	};

	header("[A] SCHEDULER ROLE, SPREAD keys = now+uniform(0,100ms)  (the real deferred-log/timer pattern)");
	bench_row("stash", true, stash_make, threads, total, Spread::Window, 100ULL * MS);
	bench_row("heap", true, heap_make, threads, total, Spread::Window, 100ULL * MS);
	bench_row("multimap", true, mm_make, threads, total, Spread::Window, 100ULL * MS);
	printf("\n");

	header("[B] HAND-OFF / STRESS, CONVERGENT keys = now()  (immediate pattern; Xapiand runs these INLINE, not via stash)");
	bench_row("stash", true, stash_make, threads, total, Spread::Convergent, 0);
	bench_row("heap", true, heap_make, threads, total, Spread::Convergent, 0);
	bench_row("deque", false, deque_make, threads, total, Spread::Convergent, 0);
	bench_row("vyukov", false, vyukov_make, threads, total, Spread::Convergent, 0);
	bench_row("sharded", false, sharded_make, threads, total, Spread::Convergent, 0);
	printf("\n");

	return 0;
}
