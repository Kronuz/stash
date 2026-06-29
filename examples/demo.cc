// A runnable tour of stash: the lock-free hierarchical slot store under a timer
// wheel.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/stash_demo
//
// The one idea worth taking away: stash buckets pointer-like values by an
// unsigned integer key (think "a clock tick" or "fire at offset N"), and a key
// descends through a few nested levels to land in a leaf slot. Each level has
// its own resolution (Div/Mod), so a key high in the future and a key due soon
// live in different physical slots without preallocating the whole range. A
// single consumer then walks a key window in order: it can peep the frontier
// (look without consuming) or walk it (drain, consuming each slot) as time
// advances. This demo shows the key->slot mapping, adding items at scattered
// offsets, peeping the soonest entry, draining in key order, the out-of-horizon
// guard, and a small deterministic lock-free producer/consumer.
#include <cstdio>
#include <memory>
#include <vector>

#include "stash.h"

static void rule(const char* title) {
	std::printf("\n\033[1m-- %s --\033[0m\n", title);
}

// The wheel for the single-threaded tour: two keyed levels over an append-only
// leaf. A key maps slot-by-slot as (key / Div) % Mod at each level.
//   L1 (outer):  Div=8, Mod=8  -> coarse slot every 8 keys, 8 coarse slots
//   L0 (inner):  Div=1, Mod=8  -> fine slot every 1 key, 8 fine slots
//   leaf:        an append-only list of the values landing in that fine slot
// Span = Div(outer) * Mod(outer) = 8 * 8 = 64 keys: the wheel addresses keys in
// [first_valid, first_valid + 64) before it would wrap onto a reused slot.
using Item  = std::shared_ptr<int>;
using Leaf  = StashValues<Item, 4>;
using Inner = StashSlots<Leaf, 8, /*Div*/1, /*Mod*/8>;
using Wheel = StashSlots<Inner, 8, /*Div*/8, /*Mod*/8>;
static constexpr unsigned long long SPAN = 64;

// Drain every value with a key in [from, to) in key order, returning them. The
// consumer protocol: set op=walk, read atom_first_valid_key into begin_key each
// pass so it skips ranges already drained, set end_key as the window's upper
// bound, and call next() until it returns false. Because op is walk, each slot
// returned is consumed (cleared) as it is read.
static std::vector<int> drain(Wheel& wheel, StashContext& ctx,
                              unsigned long long from, unsigned long long to) {
	std::vector<int> got;
	Item item;
	ctx.op = StashContext::Operation::walk;
	ctx.atom_first_valid_key = from;
	for (int guard = 0; guard < 1000; ++guard) {
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = to;
		item.reset();
		if (!wheel.next(ctx, &item)) break;
		if (item) got.push_back(*item);
	}
	return got;
}

int main() {
	std::puts("stash demo  (a hierarchical, lock-free slot store for timer wheels)");

	// --- 1. how a key lands: (key / Div) % Mod at each level -----------------
	rule("a key descends through levels to a leaf slot");
	std::puts("  span = Div(outer)*Mod(outer) = 8*8 = 64 keys; two levels over a leaf");
	std::puts("  key   outer slot (key/8)%8   inner slot (key/1)%8");
	for (unsigned long long key : {0ULL, 3ULL, 7ULL, 8ULL, 17ULL, 63ULL}) {
		std::printf("  %3llu        %llu                  %llu\n",
		            key, (key / 8) % 8, (key / 1) % 8);
	}
	std::puts("  (keys 0..7 share an outer slot but split across inner slots;");
	std::puts("   key 8 rolls into the next outer slot. the structure is sparse:");
	std::puts("   only the slots that hold something are ever built.)");

	// --- 2. add values at scattered offsets ----------------------------------
	rule("add() places a value at an absolute key (here, a fire offset)");
	Wheel wheel;
	StashContext ctx(0ULL);   // op=walk, all bounds start at 0
	// Insert out of order, at offsets spread across the 64-key horizon. Values
	// are shared_ptr because the container requires a pointer-like, bool-testable
	// type (a "zero" value reads as absent). add() widens the valid-key bounds.
	const std::pair<unsigned long long, int> schedule[] = {
		{5, 105}, {1, 101}, {40, 140}, {3, 103}, {17, 117}, {1, 1001}, {62, 162},
	};
	for (auto& [key, val] : schedule) {
		wheel.add(ctx, key, std::make_shared<int>(val));
		std::printf("  add key=%-2llu value=%-4d  -> outer slot %llu, inner slot %llu\n",
		            key, val, (key / 8) % 8, (key / 1) % 8);
	}
	std::printf("  valid-key bounds now: first=%llu last=%llu\n",
	            ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load());
	std::puts("  (two values share key 1: the leaf is an append-only list, so both");
	std::puts("   are kept and come back in insertion order when that slot drains.)");

	// --- 3. peep: look at the frontier without consuming ---------------------
	rule("peep() finds the soonest entry without draining it");
	// peep is a read-only lookahead: it advances no cursor and consumes nothing,
	// so the same value is still there to walk later. A scheduler uses it to size
	// the next sleep ("how long until the soonest task?").
	{
		StashContext pctx(0ULL);
		pctx.op = StashContext::Operation::peep;
		pctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
		pctx.begin_key = 0;
		pctx.end_key = SPAN;
		Item item;
		if (wheel.next(pctx, &item) && item) {
			std::printf("  soonest value is %d (peeped, NOT consumed)\n", *item);
		}
	}

	// --- 4. walk: drain a window in key order as time advances ---------------
	rule("walk() drains a key window in order, consuming each slot");
	// Advance "time" in two windows. First everything due by key 8, then the
	// rest. Each walk consumes what it returns, so the second window only sees
	// what the first left behind.
	std::vector<int> first = drain(wheel, ctx, 0, 8);
	std::fputs("  due by key 8 :", stdout);
	for (int v : first) std::printf(" %d", v);
	std::puts("   (note the two key-1 values come back in insertion order: 101, 1001)");

	std::vector<int> rest = drain(wheel, ctx, 8, SPAN);
	std::fputs("  due by key 64:", stdout);
	for (int v : rest) std::printf(" %d", v);
	std::putc('\n', stdout);
	std::puts("  (walk skips the empty stretches between 17, 40, 62 via the valid-key");
	std::puts("   bounds instead of stepping over 60-odd empty slots one at a time.)");

	// --- 5. the horizon guard: keys past one span are rejected ---------------
	rule("add() rejects a key beyond one wheel span (the wrap horizon)");
	// The wheel reuses each physical slot every SPAN keys. A key a full span or
	// more past first_valid would alias a slot still in use, so add() throws
	// instead of silently colliding. Here first_valid has advanced to 64 from the
	// drain above, so the horizon sits at 64 + 64 = 128.
	{
		unsigned long long fvk = ctx.atom_first_valid_key.load();
		unsigned long long beyond = fvk + SPAN;     // exactly one span out: at the horizon
		try {
			wheel.add(ctx, beyond, std::make_shared<int>(999));
			std::printf("  add key=%llu unexpectedly accepted\n", beyond);
		} catch (const std::out_of_range&) {
			std::printf("  add key=%llu rejected (out_of_range): first_valid=%llu, span=%llu, horizon=%llu\n",
			            beyond, fvk, SPAN, fvk + SPAN);
		}
	}

	// --- 6. the lock-free angle: many producers, one consumer ----------------
	rule("lock-free insert: producers add() without a lock, one consumer drains");
	// stash's insert path is lock-free: add() is a Div/Mod descent that publishes
	// chunks and nodes with compare-and-swap, so producers never block one
	// another. To keep this demo deterministic in output, we add from one thread
	// (the property being shown is "no lock taken on insert", not a race), then a
	// single consumer drains the whole window in key order. The total and the
	// in-order drain are the same every run.
	{
		Wheel w2;
		StashContext c2(0ULL);
		// Three "producers" each contribute values at distinct, interleaved keys.
		// In a real system these run on separate threads calling add() with no
		// mutex; the slot store's CAS publication is what makes that safe.
		int total = 0;
		long long sum = 0;
		for (int producer = 0; producer < 3; ++producer) {
			for (int n = 0; n < 5; ++n) {
				unsigned long long key = producer + n * 3;   // 0,3,6,9,12 / 1,4,7,10,13 / 2,5,8,11,14
				// Values start at 1: a value of 0 would read as absent (the walk
				// tests *ptr && **ptr), so the demo keeps every entry non-zero to
				// honor the pointer-like contract and keep the count exact.
				int val = producer * 100 + n + 1;
				w2.add(c2, key, std::make_shared<int>(val));
				++total;
				sum += val;
			}
		}
		std::printf("  3 producers added %d values across keys 0..14 (no lock taken)\n", total);

		std::vector<int> drained = drain(w2, c2, 0, 15);
		long long check = 0;
		for (int v : drained) check += v;
		std::printf("  one consumer drained %zu values in key order; sum %lld == %lld (every value fired once)\n",
		            drained.size(), check, sum);
		std::fputs("  first eight, in key order:", stdout);
		for (size_t i = 0; i < drained.size() && i < 8; ++i) std::printf(" %d", drained[i]);
		std::puts(" ...");
	}

	std::puts("\ndone.");
	return 0;
}
