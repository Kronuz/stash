// Concurrent safety proof for the clean-drop reclamation.
//
// Many producer threads add() tasks at the leading edge (now + jitter) while one
// consumer walks (drains+fires) and cleans (drops subtrees behind a margin), over
// a real-time wheel that wraps many times. We assert every added task fires
// exactly once (a dropped-too-early subtree shows up as a lost task), and we run
// the whole thing under ASan / TSan to catch use-after-free and data races.
//
// The margin is a parameter:
//   - a safe margin (>> add duration): clean only drops the provably-past;
//     expect exact accounting, no sanitizer complaints.
//   - margin 0 (drop right up to now, i.e. "walk cleans at the leading edge"):
//     expect a race — lost tasks and/or a sanitizer use-after-free. That is the
//     proof that reclamation needs a safe point, so clean cannot collapse into
//     walk at the present.
//
// Build (see run script):
//   plain + -DTRACK_MEM : accounting + bounded-memory check
//   -fsanitize=address  : use-after-free / heap corruption
//   -fsanitize=thread   : data races

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <random>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#ifdef TRACK_MEM
#include <new>
static std::atomic<long long> g_live_bytes{0};
void* operator new(std::size_t n) {
	void* p = std::malloc(n + 16);
	if (!p) throw std::bad_alloc();
	*reinterpret_cast<std::size_t*>(p) = n;
	g_live_bytes.fetch_add((long long)n, std::memory_order_relaxed);
	return reinterpret_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
	if (!p) return;
	void* b = reinterpret_cast<char*>(p) - 16;
	g_live_bytes.fetch_sub((long long)*reinterpret_cast<std::size_t*>(b), std::memory_order_relaxed);
	std::free(b);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
#endif

#include "stash.h"

static constexpr unsigned long long MS = 1000000ULL;
static inline unsigned long long now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Task {
	uint64_t id;
	explicit Task(uint64_t i = 0) : id(i) {}
	explicit operator bool() const { return true; }
};
using TaskPtr = std::shared_ptr<Task>;

// Real-time wheel, span = 8*8ms = 64ms, wraps ~16x/second.
using Leaf = StashValues<TaskPtr, 8>;
using L1   = StashSlots<Leaf, 8, 1ULL * MS, 8>;
using Wheel = StashSlots<L1, 8, 8ULL * MS, 8>;
static constexpr unsigned long long SPAN = 64ULL * MS;   // 8 * 8ms

int main(int argc, char** argv) {
	int nproducers = argc > 1 ? atoi(argv[1]) : 8;
	double seconds = argc > 2 ? atof(argv[2]) : 2.0;
	unsigned long long margin = (argc > 3 ? (unsigned long long)atoll(argv[3]) : 32) * MS;
	unsigned long long pause_ns = argc > 4 ? (unsigned long long)atoll(argv[4]) : 0;  // throttle per add
	int use_reclaim = argc > 5 ? atoi(argv[5]) : 1;  // B: 1 = announce/clamp/hazard on

	// Unbuffered output (so nothing is lost on a crash) + a watchdog that turns an
	// infinite loop (structural corruption) into a reported FAIL instead of a hang.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	double wd_secs = seconds + 12.0;
	std::thread([wd_secs]() {
		std::this_thread::sleep_for(std::chrono::duration<double>(wd_secs));
		std::fprintf(stderr, "WATCHDOG: run exceeded %.0fs - infinite loop / structural corruption\n", wd_secs);
		std::printf("  RESULT: FAIL - watchdog timeout (hang == corruption)\n");
		std::fflush(stdout);
		std::_Exit(2);
	}).detach();

	auto wheel = std::make_unique<Wheel>();
	StashContext ctx(now_ns());     // shared by producers (add) and consumer (walk)
	StashContext cctx(now_ns());    // consumer-only (clean)
	stash_reclaim::Domain<> domain; // B: shared announce registry
	if (use_reclaim) { ctx.reclaim = &domain; cctx.reclaim = &domain; }

	std::atomic<uint64_t> uid_gen{0};
	std::atomic<uint64_t> added_count{0}, added_xor{0};
	uint64_t fired_count = 0, fired_xor = 0;     // consumer-only
	std::atomic<int> phase{0};                   // 0=run, 1=draining
	std::atomic<bool> producers_done{false};
#ifdef TRACK_MEM
	std::atomic<long long> peak_live{0};
#endif

	auto producer = [&](int tid) {
		std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)tid);
		std::uniform_int_distribution<unsigned long long> jitter(0, SPAN / 8);   // small horizon: jitter+margin << span (no aliasing)
		while (phase.load(std::memory_order_acquire) == 0) {
			uint64_t id = uid_gen.fetch_add(1, std::memory_order_relaxed);
			auto t = std::make_shared<Task>(id);
			unsigned long long key = now_ns() + jitter(rng);
			try {
				wheel->add(ctx, key, t);
				added_count.fetch_add(1, std::memory_order_relaxed);
				added_xor.fetch_xor(id, std::memory_order_relaxed);
			} catch (const std::out_of_range&) {
				// beyond horizon: not inserted, not accounted
			}
			if (pause_ns) { auto u = now_ns() + pause_ns; while (now_ns() < u) {} }
		}
	};

	auto fire = [&](const TaskPtr& t) { ++fired_count; fired_xor ^= t->id; };

	auto consumer = [&]() {
		TaskPtr out;
		while (phase.load(std::memory_order_acquire) == 0) {
			ctx.op = StashContext::Operation::walk;
			ctx.begin_key = ctx.atom_first_valid_key.load();
			ctx.end_key = now_ns();
			while (wheel->next(ctx, &out)) { if (out) fire(out); out.reset(); }

			auto bk = ctx.atom_first_valid_key.load();
			if (bk < cctx.atom_first_valid_key.load()) cctx.atom_first_valid_key = bk;
			cctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
			cctx.op = StashContext::Operation::clean;
			cctx.begin_key = cctx.atom_first_valid_key.load();
			cctx.end_key = now_ns() - margin;
			while (wheel->next(cctx, &out)) { out.reset(); }
#ifdef TRACK_MEM
			auto lb = g_live_bytes.load();
			auto pk = peak_live.load();
			while (lb > pk && !peak_live.compare_exchange_weak(pk, lb)) {}
#endif
		}
		// Producers told to stop; wait until they're joined, then drain for a
		// bounded wall-clock window long enough that every scheduled task is due.
		// Bounded so a lost/stuck task reports as a loss instead of hanging.
		while (!producers_done.load(std::memory_order_acquire)) {}
		auto drain_until = now_ns() + 3ULL * SPAN + margin;
		while (now_ns() < drain_until) {
			ctx.op = StashContext::Operation::walk;
			ctx.begin_key = ctx.atom_first_valid_key.load();
			ctx.end_key = now_ns();
			while (wheel->next(ctx, &out)) { if (out) fire(out); out.reset(); }
		}
	};

	std::thread cons(consumer);
	std::vector<std::thread> prod;
	for (int i = 0; i < nproducers; ++i) prod.emplace_back(producer, i);

	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	phase.store(1, std::memory_order_release);
	for (auto& p : prod) p.join();
	producers_done.store(true, std::memory_order_release);
	cons.join();

	uint64_t added = added_count.load(), axor = added_xor.load();
	bool ok = (fired_count == added) && (fired_xor == axor);
	printf("producers=%d  seconds=%.1f  margin=%llums\n", nproducers, seconds, margin / MS);
	printf("  added=%llu fired=%llu  xor(add)=%016llx xor(fire)=%016llx\n",
	       (unsigned long long)added, (unsigned long long)fired_count,
	       (unsigned long long)axor, (unsigned long long)fired_xor);
#ifdef TRACK_MEM
	printf("  peak live bytes=%lld  (bounded by working set, not by added)\n", peak_live.load());
#endif
	printf("  RESULT: %s\n", ok ? "OK - every task fired exactly once" :
	       "FAIL - tasks lost or double-fired (reclamation released something in use)");
	return ok ? 0 : 1;
}
