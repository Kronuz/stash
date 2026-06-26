// Smoke test + regression tests for the standalone stash library.
//
// Drives the lock-free hierarchical slot store the way SchedulerQueue does: a
// keyed wheel (StashSlots) over an append-only leaf (StashValues), with a
// deterministic integer "clock". Values are pointer-like (shared_ptr), as the
// container requires.
//
// The regression tests below cover two unsigned-wrap correctness bugs in the
// key arithmetic:
//   - get_dec_base_key underflowing to ~2^64 on low keys and corrupting
//     first_valid_key.
//   - get_end_base_key overflowing for keys high in the 64-bit range and
//     breaking the overflow guard in add().
//
// Build: c++ -std=c++17 -I.. test.cc -o test && ./test
#include <cassert>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>
#include "stash.h"

// Deterministic key source (the vestigial _CurrentKey template parameter).
static unsigned long long now() { return 0; }

// The smoke test types: a single keyed level (Div=1, Mod=8) over a leaf.
using Item  = std::shared_ptr<int>;
using Leaf  = StashValues<Item, 4, &now>;
using Wheel = StashSlots<Leaf, 8, &now, /*Div*/1, /*Mod*/8, /*Ring*/true>;

// Original smoke test: insert at small keys, walk in order, consume.
static void test_smoke() {
	Wheel wheel;
	StashContext ctx(0ULL);   // op=walk, begin/end/first/last = 0

	// Insert three non-zero values at keys 1, 2, 3 (within [0, Div*Mod) = [0, 8)).
	wheel.add(ctx, 1, std::make_shared<int>(11));
	wheel.add(ctx, 2, std::make_shared<int>(22));
	wheel.add(ctx, 3, std::make_shared<int>(33));

	// Walk everything due up to key 6, collecting in key order (consumes slots).
	std::vector<int> got;
	Item item;
	ctx.op = StashContext::Operation::walk;
	for (int guard = 0; guard < 100; ++guard) {
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = 6;
		item.reset();
		if (!wheel.next(ctx, &item)) break;
		if (item) got.push_back(*item);
	}

	assert(got.size() == 3);
	assert(got[0] == 11 && got[1] == 22 && got[2] == 33);

	std::printf("stash OK: walked %zu values in key order: %d %d %d\n",
	            got.size(), got[0], got[1], got[2]);
}

// Bug 1 regression: the low-key path must not underflow first_valid_key.
//
// When a walk finds a value at a key in [0, _Div), ret_next computes
// get_dec_base_key(begin_key) with begin_key still in that range. Before the
// fix that was get_base_key - _Div, which wraps to ~2^64, and the CAS loop in
// next() publishes that wrapped value into atom_first_valid_key because it is
// "greater than" the current bound. After the fix the floor is 0, so the
// bound stays sane.
static void test_low_key_no_underflow() {
	const unsigned long long huge =
		std::numeric_limits<unsigned long long>::max() / 2;

	// Find a value at key 0: begin_key stays 0 (no bump, since found), so
	// ret_next calls get_dec_base_key(0) — the exact condition that underflowed
	// and published ~2^64 into first_valid_key.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		wheel.add(ctx, 0, std::make_shared<int>(100));

		ctx.op = StashContext::Operation::walk;
		ctx.begin_key = 0;
		ctx.end_key = 8;
		Item item;
		bool found = wheel.next(ctx, &item);

		assert(found && item && *item == 100);
		auto fvk = ctx.atom_first_valid_key.load();
		// Before the fix this was 2^64-1; after the fix it stays at the floor (0).
		assert(fvk < huge && "first_valid_key underflowed on found-at-key-0 walk");
		assert(fvk == 0 && "first_valid_key should floor at 0 at the low end");
	}

	// Insert at key 0 and a few small keys, then walk-consume them. Across the
	// whole drain the published first_valid_key bound must never go huge.
	{
		Wheel wheel;
		StashContext ctx(0ULL);

		wheel.add(ctx, 0, std::make_shared<int>(100));
		wheel.add(ctx, 1, std::make_shared<int>(101));
		wheel.add(ctx, 2, std::make_shared<int>(102));

		std::vector<int> got;
		Item item;
		ctx.op = StashContext::Operation::walk;
		for (int guard = 0; guard < 100; ++guard) {
			ctx.begin_key = ctx.atom_first_valid_key.load();
			ctx.end_key = 8;
			item.reset();
			if (!wheel.next(ctx, &item)) break;
			if (item) got.push_back(*item);

			// The bound must stay sane on every iteration, not just at the end.
			auto fvk = ctx.atom_first_valid_key.load();
			assert(fvk < huge && "first_valid_key underflowed mid-walk");
		}

		assert(got.size() == 3);
		assert(got[0] == 100 && got[1] == 101 && got[2] == 102);

		auto fvk = ctx.atom_first_valid_key.load();
		assert(fvk < huge && "first_valid_key underflowed after low-key drain");
	}

	std::printf("stash OK: low-key path keeps first_valid_key sane (no underflow)\n");
}

// Bug 2 regression: the span/overflow guard must not wrap for high keys.
//
// add() throws when key >= get_end_base_key(first_valid_key), i.e. when the
// key is more than one wheel span past the first valid key's base. With a key
// high in the 64-bit range, get_base_key + Div*Mod used to wrap to a small
// value, so the guard either threw on a clearly-valid key (just past
// first_valid_key) or accepted a key far beyond the horizon. After the fix the
// horizon saturates at max instead of wrapping.
static void test_high_key_span_guard() {
	const unsigned long long span = 1ULL * 8;  // _Div * _Mod for this wheel.
	// A base key so high that base + span would overflow 2^64.
	const unsigned long long high_base =
		std::numeric_limits<unsigned long long>::max() - 3;  // > MAX - span.

	// Case A: a key within one span of first_valid_key must be accepted, even
	// when first_valid_key is high enough that base + span overflows.
	{
		Wheel wheel;
		StashContext ctx(high_base);  // seeds first/last valid key = high_base.

		bool threw = false;
		try {
			// high_base itself is trivially within the horizon: with a wrapped
			// horizon the guard would reject this valid key.
			wheel.add(ctx, high_base, std::make_shared<int>(7));
		} catch (const std::out_of_range&) {
			threw = true;
		}
		assert(!threw && "valid high key wrongly rejected (span guard wrapped)");
	}

	// Case B: a key just one unit past first_valid_key, still well within a
	// span, must also be accepted near the top of the range.
	{
		Wheel wheel;
		StashContext ctx(high_base);

		bool threw = false;
		try {
			wheel.add(ctx, high_base + 1, std::make_shared<int>(8));
		} catch (const std::out_of_range&) {
			threw = true;
		}
		assert(!threw && "valid high key (fvk+1) wrongly rejected (span guard wrapped)");
	}

	// Case C: from a low first_valid_key, a key genuinely beyond the horizon
	// must still be rejected. This confirms the guard didn't get disabled; it
	// only stops wrapping. With fvk = 0 and span = 8, key 8 is the first key at
	// or past the horizon and must throw.
	{
		Wheel wheel;
		StashContext ctx(0ULL);

		bool threw = false;
		try {
			wheel.add(ctx, span, std::make_shared<int>(9));  // key == horizon.
		} catch (const std::out_of_range&) {
			threw = true;
		}
		assert(threw && "key at/beyond horizon should be rejected");
	}

	std::printf("stash OK: high-key span guard rejects beyond-horizon without wrapping\n");
}

int main() {
	test_smoke();
	test_low_key_no_underflow();
	test_high_key_span_guard();
	std::printf("all stash tests passed\n");
	return 0;
}
