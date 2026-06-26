// Scheduler shoot-out: the stash-backed wheel vs asio::steady_timer vs mutex+heap.
//
// Workload mirrors how Xapiand's scheduler actually gets hit: N producer threads
// arm timed tasks, one consumer fires them. We measure the ARM (insert) path,
// which is the contended hot path.
//
//   SPREAD      key = now + uniform(0, W)   (the real deferred-log/timer pattern)
//   CONVERGENT  key = now                    (pathological all-at-once)
//
// Asio usage note: one steady_timer per scheduled task is the honest way to ask
// Asio "run this callback at time T" — Asio has no fire-at-T primitive without a
// timer object. Pooling timers means rebuilding a scheduler on top of Asio.

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

// ---- stash wheel, Xapiand's exact config -----------------------------------
struct StashWheel {
	using Tasks = StashValues<TaskPtr, 10ULL>;
	using L1 = StashSlots<Tasks, 10ULL, 1ULL * MS, 50ULL>;
	using L2 = StashSlots<L1, 10ULL, 50ULL * MS, 10ULL>;
	using L3 = StashSlots<L2, 12ULL, 500ULL * MS, 36ULL>;
	using Wheel = StashSlots<L3, 4800ULL, 18000ULL * MS, 4800ULL>;
	StashContext ctx;
	Wheel queue;
	StashWheel() : ctx(now_ns()) {}
	void arm(int, unsigned long long key, const TaskPtr& t) {
		try { queue.add(ctx, key, t); } catch (const std::out_of_range&) {}
	}
	void walk() {
		ctx.op = StashContext::Operation::walk;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = now_ns();
		TaskPtr out;
		while (queue.next(ctx, &out)) { out.reset(); }
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
	void arm(int, unsigned long long key, const TaskPtr& t) {
		uint64_t s = seqgen.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lk(m);
		pq.push(Item{key, s, t});
	}
	void walk() {
		auto cutoff = now_ns();
		for (;;) {
			TaskPtr t;
			{ std::lock_guard<std::mutex> lk(m);
			  if (pq.empty() || pq.top().key > cutoff) break;
			  t = pq.top().t; pq.pop(); }
		}
	}
};

enum class Spread { Convergent, Window };

struct Result { double mops, p50, p99, p999; };

static Result finish(uint64_t real_total, double secs, std::vector<std::vector<uint32_t>>& lat) {
	std::vector<uint32_t> all; all.reserve(real_total);
	for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
	std::sort(all.begin(), all.end());
	auto pct = [&](double p) -> double { return all.empty() ? 0 : all[(size_t)(p * (all.size() - 1))]; };
	return Result{(double)real_total / secs / 1e6, pct(0.50), pct(0.99), pct(0.999)};
}

// ---- walk-style driver (stash, heap) ---------------------------------------
template <typename Q>
Result run_walk(Q& q, int nthreads, uint64_t total, Spread spread, unsigned long long window_ns) {
	uint64_t per = total / nthreads, real_total = per * nthreads;
	std::atomic<int> ready{0}; std::atomic<bool> go{false}, done{false};
	std::vector<std::vector<uint32_t>> lat(nthreads);
	for (auto& v : lat) v.reserve(per);

	std::thread cons([&]{ while (!go.load(std::memory_order_acquire)) {} while (!done.load(std::memory_order_acquire)) q.walk(); q.walk(); });
	auto producer = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> d(0, window_ns ? window_ns - 1 : 0);
		ready.fetch_add(1, std::memory_order_release);
		while (!go.load(std::memory_order_acquire)) {}
		for (uint64_t i = 0; i < per; ++i) {
			auto t = std::make_shared<Task>((uint64_t)tid << 40 | i);
			unsigned long long key = now_ns(); if (spread == Spread::Window) key += d(rng);
			auto t0 = std::chrono::steady_clock::now();
			q.arm(tid, key, t);
			auto t1 = std::chrono::steady_clock::now();
			lat[tid].push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		}
	};
	std::vector<std::thread> prod;
	for (int i = 0; i < nthreads; ++i) prod.emplace_back(producer, i);
	while (ready.load(std::memory_order_acquire) < nthreads) {}
	auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);
	for (auto& th : prod) th.join();
	auto end = std::chrono::steady_clock::now();
	done.store(true, std::memory_order_release); cons.join();
	return finish(real_total, std::chrono::duration<double>(end - start).count(), lat);
}

// ---- Asio driver -----------------------------------------------------------
Result run_asio(int nthreads, uint64_t total, Spread spread, unsigned long long window_ns) {
	uint64_t per = total / nthreads, real_total = per * nthreads;
	asio::io_context io;
	auto guard = asio::make_work_guard(io);
	std::atomic<uint64_t> fired{0};
	std::atomic<int> ready{0}; std::atomic<bool> go{false};
	std::vector<std::vector<uint32_t>> lat(nthreads);
	for (auto& v : lat) v.reserve(per);

	std::thread runner([&]{ io.run(); });
	auto producer = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> d(0, window_ns ? window_ns - 1 : 0);
		ready.fetch_add(1, std::memory_order_release);
		while (!go.load(std::memory_order_acquire)) {}
		for (uint64_t i = 0; i < per; ++i) {
			auto t = std::make_shared<Task>((uint64_t)tid << 40 | i);
			unsigned long long key = now_ns(); if (spread == Spread::Window) key += d(rng);
			auto t0 = std::chrono::steady_clock::now();
			auto* timer = new asio::steady_timer(io);
			timer->expires_at(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(key)));
			timer->async_wait([&fired, timer, t](const asio::error_code&) {
				fired.fetch_add(1, std::memory_order_relaxed);
				delete timer;
			});
			auto t1 = std::chrono::steady_clock::now();
			lat[tid].push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		}
	};
	std::vector<std::thread> prod;
	for (int i = 0; i < nthreads; ++i) prod.emplace_back(producer, i);
	while (ready.load(std::memory_order_acquire) < nthreads) {}
	auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);
	for (auto& th : prod) th.join();
	auto end = std::chrono::steady_clock::now();
	guard.reset();      // let pending timers drain, then io.run() returns
	runner.join();
	return finish(real_total, std::chrono::duration<double>(end - start).count(), lat);
}

int main(int argc, char** argv) {
	uint64_t total = 1000000;
	if (argc > 1) total = strtoull(argv[1], nullptr, 10);
	std::vector<int> threads = {1, 2, 4, 8, 14};

	printf("stash scheduler benchmark — total arms = %llu, 1 consumer, best-of-3\n", (unsigned long long)total);
	printf("Throughput = million arms/sec (higher better); p99 = producer arm latency\n\n");

	auto header = [&](const char* title) {
		printf("%s\n  %-10s", title, "threads");
		for (int nt : threads) printf(" | %6d", nt);
		printf("\n");
	};
	auto row = [&](const char* name, Spread sp, unsigned long long w, int which) {
		printf("  %-10s", name);
		std::vector<Result> rs;
		for (int nt : threads) {
			Result best{0,0,0,0}; double bestp99 = 1e18;
			for (int rep = 0; rep < 3; ++rep) {
				Result r;
				if (which == 0) { auto q = std::make_unique<StashWheel>(); r = run_walk(*q, nt, total, sp, w); }
				else if (which == 1) { auto q = std::make_unique<MutexHeap>(); r = run_walk(*q, nt, total, sp, w); }
				else { r = run_asio(nt, total, sp, w); }
				if (r.mops > best.mops) best = r;
				if (r.p99 < bestp99) bestp99 = r.p99;
			}
			best.p99 = bestp99;
			rs.push_back(best);
		}
		for (auto& r : rs) printf(" | %6.2f", r.mops);
		printf("\n  %-10s", "");
		for (auto& r : rs) printf(" | %5.0fns", r.p99);
		printf("   (p99)\n");
	};

	header("[A] SCHEDULER, SPREAD keys = now+uniform(0,100ms)  (the real pattern)");
	row("stash", Spread::Window, 100ULL * MS, 0);
	row("heap", Spread::Window, 100ULL * MS, 1);
	row("asio", Spread::Window, 100ULL * MS, 2);
	printf("\n");

	header("[B] SCHEDULER, CONVERGENT keys = now()  (pathological all-at-once)");
	row("stash", Spread::Convergent, 0, 0);
	row("heap", Spread::Convergent, 0, 1);
	row("asio", Spread::Convergent, 0, 2);
	printf("\n");
	return 0;
}
