// Longevity / wraparound reclamation test.
//
// Question: over a long run, does the wheel's structure stay bounded, or does it
// grow with total inserts? We drive a small wheel through many wraparounds and
// print live heap bytes over time. A LOAD phase (steady inserts) is followed by a
// QUIET phase (no inserts, keep walking + cleaning) to see whether memory is
// reclaimed once the load stops.
//
//   plateau during load + drop during quiet  => bounded, reclaimed (no leak)
//   linear climb with total inserts           => leak (structure never freed)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <memory>
#include <random>

static std::atomic<long long> g_live_bytes{0};
static std::atomic<long long> g_live_count{0};

void* operator new(std::size_t n) {
	void* p = std::malloc(n + 16);
	if (!p) throw std::bad_alloc();
	*reinterpret_cast<std::size_t*>(p) = n;
	g_live_bytes.fetch_add((long long)n, std::memory_order_relaxed);
	g_live_count.fetch_add(1, std::memory_order_relaxed);
	return reinterpret_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
	if (!p) return;
	void* base = reinterpret_cast<char*>(p) - 16;
	std::size_t n = *reinterpret_cast<std::size_t*>(base);
	g_live_bytes.fetch_sub((long long)n, std::memory_order_relaxed);
	g_live_count.fetch_sub(1, std::memory_order_relaxed);
	std::free(base);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#include "stash.h"

struct Task {
	uint64_t id;
	explicit Task(uint64_t i = 0) : id(i) {}
	explicit operator bool() const { return true; }
};
using TaskPtr = std::shared_ptr<Task>;

// Small wheel: two keyed levels over a leaf. Span = 8*8 = 64 units.
using Leaf  = StashValues<TaskPtr, 4>;
using L1    = StashSlots<Leaf, 4, 1ULL, 8ULL>;
using Wheel = StashSlots<L1, 4, 8ULL, 8ULL>;
static constexpr unsigned long long SPAN = 64;

static void simulate(bool do_clean) {
	const unsigned long long BASE = 1000;
	const int load_spans = 400, quiet_spans = 150;
	auto wheel = std::make_unique<Wheel>();
	StashContext ctx(BASE);
	StashContext cctx(BASE);
	std::mt19937_64 rng(12345);
	std::uniform_int_distribution<unsigned long long> delay(0, 6);
	std::uniform_int_distribution<int> count(1, 4);

	uint64_t inserted = 0, fired = 0;
	long long baseline = g_live_bytes.load();

	printf("clean %s\n", do_clean ? "ENABLED" : "disabled");
	printf("  %-6s %-7s | %10s | %10s | %8s\n", "span", "phase", "inserted", "live(B)", "allocs");

	for (int span = 0; span < load_spans + quiet_spans; ++span) {
		bool loading = span < load_spans;
		for (unsigned long long tick = 0; tick < SPAN; ++tick) {
			unsigned long long t = BASE + (unsigned long long)span * SPAN + tick;
			if (loading) {
				int k = count(rng);
				for (int j = 0; j < k; ++j) {
					unsigned long long key = t + delay(rng);
					try { wheel->add(ctx, key, std::make_shared<Task>(inserted)); ++inserted; }
					catch (const std::out_of_range&) {}
				}
			}
			ctx.op = StashContext::Operation::walk;
			ctx.begin_key = ctx.atom_first_valid_key.load();
			ctx.end_key = t;
			TaskPtr out;
			while (wheel->next(ctx, &out)) { if (out) ++fired; out.reset(); }
			if (do_clean) {
				auto bk = ctx.atom_first_valid_key.load();
				if (bk < cctx.atom_first_valid_key.load()) cctx.atom_first_valid_key = bk;
				cctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
				cctx.op = StashContext::Operation::clean;
				cctx.begin_key = cctx.atom_first_valid_key.load();
				cctx.end_key = t - 2;
				TaskPtr c;
				while (wheel->next(cctx, &c)) { c.reset(); }
			}
		}
		if (span % 50 == 0 || span == load_spans - 1 || span == load_spans || span == load_spans + quiet_spans - 1) {
			printf("  %-6d %-7s | %10llu | %10lld | %8lld\n",
			       span, loading ? "LOAD" : "quiet",
			       (unsigned long long)inserted, g_live_bytes.load() - baseline, g_live_count.load());
		}
	}
	ctx.op = StashContext::Operation::walk;
	ctx.begin_key = ctx.atom_first_valid_key.load();
	ctx.end_key = BASE + (unsigned long long)(load_spans + quiet_spans + 1) * SPAN;
	TaskPtr out;
	while (wheel->next(ctx, &out)) { if (out) ++fired; out.reset(); }
	printf("  -> inserted=%llu fired=%llu  live after final drain=%lld bytes (structure still held)\n\n",
	       (unsigned long long)inserted, (unsigned long long)fired, g_live_bytes.load() - baseline);
}

int main() {
	printf("Longevity: small wheel span=%llu, 400 LOAD spans then 150 QUIET spans.\n", SPAN);
	printf("Watch live(B): does it plateau (bounded) or climb with inserted (leak)?\n\n");
	simulate(false);
	simulate(true);
	return 0;
}
