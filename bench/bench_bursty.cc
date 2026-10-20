// Many mostly-idle producers: Xapiand's deferred-logging shape.
//
// Hundreds to thousands of request threads each add a short burst of tasks and
// then block (here, sleep) for a while, so only a few are inside add() at any
// instant. One consumer walks and cleans as SchedulerQueue does. Reports insert
// throughput, add() latency percentiles, how often the announce claim missed its
// hinted slot or fell back to the overflow counter, and lost tasks (must be 0).
//
//   bench_bursty [SECONDS] [BURST] [SLEEP_MIN_US] [SLEEP_MAX_US] THREADS...
//   bench_bursty 2 8 500 2000 256 1024

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

// Count the announce claim outcomes per thread (a no-op on builds without it).
struct AnnounceStats {
	uint64_t claims = 0, misses = 0, probes = 0, overflows = 0;
};
static thread_local AnnounceStats t_stats;
#define STASH_ANNOUNCE_STAT(p, o) \
	(++t_stats.claims, t_stats.misses += (p) != 0 && !(o), t_stats.probes += (o) ? 0 : (p), t_stats.overflows += (o))

#include "stash.h"

static constexpr unsigned long long MS = 1000000ULL;

static inline unsigned long long now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Task {
	uint64_t id;
	explicit Task(uint64_t i) : id(i) {}
	explicit operator bool() const { return true; }
};
using TaskPtr = std::shared_ptr<Task>;

// Xapiand's wheel, as in scheduler.h.
using Tasks = StashValues<TaskPtr, 10ULL>;
using L1 = StashSlots<Tasks, 10ULL, 1ULL * MS, 50ULL>;
using L2 = StashSlots<L1, 10ULL, 50ULL * MS, 10ULL>;
using L3 = StashSlots<L2, 12ULL, 500ULL * MS, 36ULL>;
using Wheel = StashSlots<L3, 4800ULL, 18000ULL * MS, 4800ULL>;

struct Result {
	double adds_per_sec;
	uint64_t added, lost;
	double p50, p99, p999;
	AnnounceStats stats;
};

static Result run(int nthreads, double seconds, int burst, unsigned sleep_min_us, unsigned sleep_max_us) {
	auto wheel = std::make_unique<Wheel>();
	StashContext ctx(now_ns());
	StashContext cctx(now_ns());

	std::atomic<bool> start{false}, stop{false};
	std::atomic<uint64_t> added{0};
	std::atomic<int> producers_left{nthreads};
	std::vector<std::vector<uint32_t>> lat(nthreads);
	std::vector<AnnounceStats> stats(nthreads);

	uint64_t fired = 0;
	std::thread consumer([&] {
		TaskPtr out;
		auto walk = [&] {
			ctx.op = StashContext::Operation::walk;
			ctx.pending_floor = StashContext::IDLE;
			ctx.begin_key = ctx.atom_first_valid_key.load();
			ctx.end_key = now_ns();
			while (wheel->next(ctx, &out)) { if (out) ++fired; out.reset(); }
		};
		while (producers_left.load(std::memory_order_acquire) > 0) {
			walk();
			auto f = ctx.atom_first_valid_key.load();
			if (f < cctx.atom_first_valid_key.load()) cctx.atom_first_valid_key = f;
			cctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
			cctx.op = StashContext::Operation::clean;
			cctx.begin_key = cctx.atom_first_valid_key.load();
			cctx.end_key = ctx.atom_first_valid_key.load();
			while (wheel->next(cctx, &out)) { out.reset(); }
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		auto cap = now_ns() + 10000ULL * MS;   // keys lead by up to 100 ms
		while (fired < added.load() && now_ns() < cap) {
			walk();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	});

	std::vector<std::thread> producers;
	producers.reserve(nthreads);
	for (int t = 0; t < nthreads; ++t) {
		producers.emplace_back([&, t] {
			std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)t);
			std::uniform_int_distribution<unsigned long long> lead(0, 100ULL * MS);
			std::uniform_int_distribution<unsigned> nap(sleep_min_us, sleep_max_us);
			auto& my = lat[t];
			my.reserve(4096);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			uint64_t id = (uint64_t)t << 40;
			while (!stop.load(std::memory_order_relaxed)) {
				for (int b = 0; b < burst; ++b) {
					auto task = std::make_shared<Task>(++id);
					auto t0 = now_ns();
					try {
						wheel->add(ctx, t0 + lead(rng), task);
						auto dt = now_ns() - t0;
						my.push_back(dt > UINT32_MAX ? UINT32_MAX : (uint32_t)dt);
						added.fetch_add(1, std::memory_order_relaxed);
					} catch (const std::out_of_range&) {}
				}
				std::this_thread::sleep_for(std::chrono::microseconds(nap(rng)));
			}
			stats[t] = t_stats;
			t_stats = {};
			producers_left.fetch_sub(1, std::memory_order_release);
		});
	}

	auto t0 = now_ns();
	start.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	stop.store(true);
	for (auto& p : producers) p.join();
	auto elapsed = (now_ns() - t0) / 1e9;
	consumer.join();

	std::vector<uint32_t> all;
	for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
	auto pct = [&](double p) -> double {
		if (all.empty()) return 0;
		size_t k = std::min(all.size() - 1, (size_t)(p * (all.size() - 1)));
		std::nth_element(all.begin(), all.begin() + k, all.end());
		return all[k];
	};
	Result r{};
	r.added = added.load();
	r.lost = r.added - fired;
	r.adds_per_sec = r.added / elapsed;
	r.p50 = pct(0.50);
	r.p99 = pct(0.99);
	r.p999 = pct(0.999);
	for (auto& s : stats) {
		r.stats.claims += s.claims;
		r.stats.misses += s.misses;
		r.stats.probes += s.probes;
		r.stats.overflows += s.overflows;
	}
	return r;
}

int main(int argc, char** argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 2.0;
	int burst = argc > 2 ? atoi(argv[2]) : 8;
	unsigned sleep_min = argc > 3 ? (unsigned)atoi(argv[3]) : 500;
	unsigned sleep_max = argc > 4 ? (unsigned)atoi(argv[4]) : 2000;
	std::vector<int> threads;
	for (int i = 5; i < argc; ++i) threads.push_back(atoi(argv[i]));
	if (threads.empty()) threads = {256, 1024};

	printf("bench_bursty: %.1fs, bursts of %d adds, sleep %u-%u us between bursts, keys now+uniform(0,100ms)\n",
	       seconds, burst, sleep_min, sleep_max);
	printf("threads |    adds/s |   p50 ns |   p99 ns |  p999 ns | hint miss | probes/miss | overflow | lost\n");
	int failed = 0;
	for (int n : threads) {
		auto r = run(n, seconds, burst, sleep_min, sleep_max);
		auto& s = r.stats;
		char miss[32] = "n/a", probes[32] = "n/a", over[32] = "n/a";
		if (s.claims) {
			snprintf(miss, sizeof miss, "%.4f%%", 100.0 * s.misses / s.claims);
			snprintf(probes, sizeof probes, "%.2f", s.misses ? (double)s.probes / s.misses : 0.0);
			snprintf(over, sizeof over, "%llu", (unsigned long long)s.overflows);
		}
		printf("%7d | %9.0f | %8.0f | %8.0f | %8.0f | %9s | %11s | %8s | %llu\n",
		       n, r.adds_per_sec, r.p50, r.p99, r.p999, miss, probes, over, (unsigned long long)r.lost);
		failed += r.lost != 0;
	}
	return failed ? 1 : 0;
}
