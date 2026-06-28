// Steady-state, bounded-depth scheduler benchmark.
//
// The other benchmarks let the pending set grow to millions, which inflates a
// heap's O(log n) per insert far past what a real scheduler ever sees. This one
// holds the live depth near a target D (dozens to thousands of pending timers,
// the regime a real scheduler actually lives in) and measures the per-insert
// (arm) LATENCY at that depth.
//
// Why latency, not throughput: at a bounded depth, sustained throughput is just
// the drain rate (the offered load), identical for every structure. What differs
// is what one insert COSTS at depth D, and its tail under contention. That is the
// number the queue-depth caveat is about.
//
// Many producers arm timed tasks; one consumer fires them; a token cap holds the
// live depth near D (producers stop arming when live >= D, resume as the consumer
// drains). Keys are now + uniform(0, W) so items mature and churn.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>

#include "stash.h"

#define ASIO_STANDALONE
#include <asio.hpp>

static constexpr unsigned long long MS = 1000000ULL;

static inline unsigned long long now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Task {
	uint64_t id;
	std::atomic<bool> active;
	explicit Task(uint64_t i = 0) : id(i), active(true) {}
	explicit operator bool() const { return active.load(std::memory_order_relaxed); }
};
using TaskPtr = std::shared_ptr<Task>;

// ---- stash wheel, current (param-free) signatures --------------------------
struct StashWheel {
	using Tasks = StashValues<TaskPtr, 10ULL>;
	using L1 = StashSlots<Tasks, 10ULL, 1ULL * MS, 50ULL>;
	using L2 = StashSlots<L1, 10ULL, 50ULL * MS, 10ULL>;
	using L3 = StashSlots<L2, 12ULL, 500ULL * MS, 36ULL>;
	using Wheel = StashSlots<L3, 4800ULL, 18000ULL * MS, 4800ULL>;
	StashContext ctx;
	Wheel queue;
	StashWheel() : ctx(now_ns()) {}
	void arm(unsigned long long key, const TaskPtr& t) {
		try { queue.add(ctx, key, t); } catch (const std::out_of_range&) {}
	}
	int drain_due() {
		ctx.op = StashContext::Operation::walk;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = now_ns();
		int n = 0;
		TaskPtr out;
		while (queue.next(ctx, &out)) { if (out) ++n; out.reset(); }
		return n;
	}
};

// ---- mutex + binary heap ---------------------------------------------------
struct MutexHeap {
	struct Item { unsigned long long key; uint64_t seq; TaskPtr t; };
	struct Cmp { bool operator()(const Item& a, const Item& b) const {
		return a.key != b.key ? a.key > b.key : a.seq > b.seq; } };
	std::mutex m;
	std::priority_queue<Item, std::vector<Item>, Cmp> pq;
	std::atomic<uint64_t> seqgen{0};
	void arm(unsigned long long key, const TaskPtr& t) {
		uint64_t s = seqgen.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lk(m);
		pq.push(Item{key, s, t});
	}
	int drain_due() {
		auto cutoff = now_ns();
		int n = 0;
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

struct Stat { double p50, p99, p999, ops_per_s, avg_depth; };

static Stat finish(uint64_t produced, double secs, std::vector<std::vector<uint32_t>>& lat,
                   double avg_depth) {
	std::vector<uint32_t> all;
	for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
	std::sort(all.begin(), all.end());
	auto pct = [&](double p) -> double { return all.empty() ? 0 : all[(size_t)(p * (all.size() - 1))]; };
	return Stat{pct(0.50), pct(0.99), pct(0.999), (double)produced / secs, avg_depth};
}

// ---- walk-style driver (stash, heap): hold depth near D --------------------
template <typename Q>
Stat run_steady_walk(int nthreads, int D, int run_ms, unsigned long long W) {
	Q q;
	std::atomic<long> live{0};
	std::atomic<bool> stop{false}, go{false};
	std::atomic<uint64_t> produced{0};
	std::vector<std::vector<uint32_t>> lat(nthreads);

	std::thread cons([&]{
		while (!go.load(std::memory_order_acquire)) {}
		while (!stop.load(std::memory_order_acquire)) {
			int drained = q.drain_due();
			if (drained) live.fetch_sub(drained, std::memory_order_acq_rel);
		}
	});

	auto prod = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> d(0, W ? W - 1 : 0);
		while (!go.load(std::memory_order_acquire)) {}
		while (!stop.load(std::memory_order_acquire)) {
			while (live.load(std::memory_order_acquire) >= D && !stop.load(std::memory_order_acquire)) {}
			if (stop.load(std::memory_order_acquire)) break;
			auto t = std::make_shared<Task>(0);
			unsigned long long key = now_ns() + d(rng);
			auto t0 = std::chrono::steady_clock::now();
			q.arm(key, t);
			auto t1 = std::chrono::steady_clock::now();
			lat[tid].push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
			live.fetch_add(1, std::memory_order_acq_rel);
			produced.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> ps;
	for (int i = 0; i < nthreads; ++i) ps.emplace_back(prod, i);
	auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);

	// run for run_ms, sampling depth
	uint64_t depth_sum = 0, depth_n = 0;
	auto deadline = start + std::chrono::milliseconds(run_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		depth_sum += (uint64_t)std::max(0L, live.load(std::memory_order_relaxed));
		++depth_n;
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	auto end = std::chrono::steady_clock::now();
	stop.store(true, std::memory_order_release);
	for (auto& th : ps) th.join();
	cons.join();

	double secs = std::chrono::duration<double>(end - start).count();
	return finish(produced.load(), secs, lat, depth_n ? (double)depth_sum / depth_n : 0);
}

// ---- Asio driver: outstanding timers held near D ---------------------------
Stat run_steady_asio(int nthreads, int D, int run_ms, unsigned long long W) {
	asio::io_context io;
	auto guard = asio::make_work_guard(io);
	std::atomic<long> live{0};
	std::atomic<bool> stop{false}, go{false};
	std::atomic<uint64_t> produced{0};
	std::vector<std::vector<uint32_t>> lat(nthreads);

	std::thread runner([&]{ io.run(); });

	auto prod = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> d(0, W ? W - 1 : 0);
		while (!go.load(std::memory_order_acquire)) {}
		while (!stop.load(std::memory_order_acquire)) {
			while (live.load(std::memory_order_acquire) >= D && !stop.load(std::memory_order_acquire)) {}
			if (stop.load(std::memory_order_acquire)) break;
			auto t = std::make_shared<Task>(0);
			unsigned long long key = now_ns() + d(rng);
			auto t0 = std::chrono::steady_clock::now();
			auto* timer = new asio::steady_timer(io);
			timer->expires_at(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(key)));
			timer->async_wait([&live, timer, t](const asio::error_code&) {
				live.fetch_sub(1, std::memory_order_acq_rel);
				delete timer;
			});
			auto t1 = std::chrono::steady_clock::now();
			lat[tid].push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
			live.fetch_add(1, std::memory_order_acq_rel);
			produced.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> ps;
	for (int i = 0; i < nthreads; ++i) ps.emplace_back(prod, i);
	auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);

	uint64_t depth_sum = 0, depth_n = 0;
	auto deadline = start + std::chrono::milliseconds(run_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		depth_sum += (uint64_t)std::max(0L, live.load(std::memory_order_relaxed));
		++depth_n;
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	auto end = std::chrono::steady_clock::now();
	stop.store(true, std::memory_order_release);
	for (auto& th : ps) th.join();
	guard.reset();
	io.stop();
	runner.join();

	double secs = std::chrono::duration<double>(end - start).count();
	return finish(produced.load(), secs, lat, depth_n ? (double)depth_sum / depth_n : 0);
}

int main(int argc, char** argv) {
	int run_ms = 600;
	if (argc > 1) run_ms = atoi(argv[1]);
	std::vector<int> depths = {64, 1024, 16384};
	std::vector<int> threads = {1, 4, 14};
	unsigned long long W = 2ULL * MS;   // keys spread over a 2ms window

	printf("Steady-state, bounded depth. Per-insert (arm) latency at a held depth.\n");
	printf("run=%dms/config, keys = now+uniform(0,2ms), best-of-2. p50/p99 in ns; depth = achieved avg.\n\n", run_ms);

	for (int D : depths) {
		printf("==== target depth D = %d ====\n", D);
		printf("  %-8s | %-7s | %10s | %10s | %10s | %8s\n", "struct", "threads", "p50(ns)", "p99(ns)", "p999(ns)", "depth");
		auto row = [&](const char* name, int which) {
			for (int nt : threads) {
				Stat best{0,0,0,0,0}; double bestp99 = 1e18;
				for (int rep = 0; rep < 2; ++rep) {
					Stat s = (which == 0) ? run_steady_walk<StashWheel>(nt, D, run_ms, W)
					       : (which == 1) ? run_steady_walk<MutexHeap>(nt, D, run_ms, W)
					                      : run_steady_asio(nt, D, run_ms, W);
					if (s.p99 < bestp99) { bestp99 = s.p99; best = s; }
				}
				printf("  %-8s | %-7d | %10.0f | %10.0f | %10.0f | %8.0f\n",
				       name, nt, best.p50, best.p99, best.p999, best.avg_depth);
			}
		};
		row("stash", 0);
		row("heap", 1);
		row("asio", 2);
		printf("\n");
	}
	return 0;
}
