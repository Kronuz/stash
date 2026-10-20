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
#include <new>
#include <vector>

// The stats hooks, counted here to check where they fire.
static unsigned long long g_overflows = 0, g_held_overflow = 0, g_held_far = 0;
#define STASH_ANNOUNCE_STAT(probes, overflowed) (g_overflows += (overflowed))
#define STASH_HELD_STAT(overflow, far_pin) (g_held_overflow += (overflow), g_held_far += (far_pin))
// Fails the g_fail_at-th allocation (counting from 1) with std::bad_alloc; 0 = off.
static int g_fail_at = 0;
#define STASH_ALLOC_HOOK() (g_fail_at && --g_fail_at == 0 ? throw std::bad_alloc() : (void)0)
#include "stash.h"

// The smoke test types: a single keyed level (Div=1, Mod=8) over a leaf.
using Item  = std::shared_ptr<int>;
using Leaf  = StashValues<Item, 4>;
using Wheel = StashSlots<Leaf, 8, /*Div*/1, /*Mod*/8>;

// Original smoke test: insert at small keys, walk in order, consume.
// walk, peep and clean are noexcept for values whose copy, bool tests and
// destruction cannot throw (shared_ptr), and not for one whose copy can.
static_assert(noexcept(std::declval<Wheel&>().next(std::declval<StashContext&>(), std::declval<Item*>())));
struct ThrowingCopy {
	int v = 1;
	ThrowingCopy& operator=(const ThrowingCopy&) noexcept(false) { return *this; }
	explicit operator bool() const noexcept { return true; }
	int operator*() const noexcept { return v; }
};
static_assert(!noexcept(std::declval<StashSlots<StashValues<ThrowingCopy, 4>, 8, 1, 8>&>().next(
    std::declval<StashContext&>(), std::declval<ThrowingCopy*>())));

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
// add() returns false when key >= get_end_base_key(first_valid_key), i.e. when the
// key is more than one wheel span past the first valid key's base. With a key
// high in the 64-bit range, get_base_key + Div*Mod used to wrap to a small
// value, so the guard either rejected a clearly-valid key (just past
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

		// high_base itself is trivially within the horizon: with a wrapped
		// horizon the guard would reject this valid key.
		bool added = wheel.add(ctx, high_base, std::make_shared<int>(7));
		assert(added && "valid high key wrongly rejected (span guard wrapped)");
		(void)added;
	}

	// Case B: a key just one unit past first_valid_key, still well within a
	// span, must also be accepted near the top of the range.
	{
		Wheel wheel;
		StashContext ctx(high_base);

		bool added = wheel.add(ctx, high_base + 1, std::make_shared<int>(8));
		assert(added && "valid high key (fvk+1) wrongly rejected (span guard wrapped)");
		(void)added;
	}

	// Case C: from a low first_valid_key, a key genuinely beyond the horizon
	// must still be rejected. This confirms the guard didn't get disabled; it
	// only stops wrapping. With fvk = 0 and span = 8, key 8 is the first key at
	// or past the horizon: add returns false and stores nothing.
	{
		Wheel wheel;
		StashContext ctx(0ULL);

		auto value = std::make_shared<int>(9);
		bool added = wheel.add(ctx, span, value);  // key == horizon.
		assert(!added && "key at/beyond horizon should be rejected");
		assert(value.use_count() == 1 && "a rejected add kept a copy of the value");
		assert(ctx.atom_last_valid_key.load() == 0 && "a rejected add published a bound");
		for (auto& a : ctx.announce) {
			assert(a.key.load() == StashContext::IDLE && a.far[0].load() == 0 && a.far[1].load() == 0 &&
			       "a rejected add left its announce behind");
		}
		ctx.op = StashContext::Operation::peep;
		ctx.begin_key = 0;
		ctx.end_key = 0;
		Item item;
		assert(!wheel.next(ctx, &item) && "a rejected add stored a value");
		(void)added;
	}

	std::printf("stash OK: high-key span guard rejects beyond-horizon without wrapping\n");
}

// Descent-window regressions. An insert is in flight from its announce until
// its value is in place; the walk must not advance first_valid past it (else it
// skips the task and clean frees the leaf under the producer), and an insert
// keyed below the floor of a walk that already ran must still fire.
static void drain(Wheel& wheel, StashContext& ctx, unsigned long long end, std::vector<int>& got) {
	Item item;
	ctx.op = StashContext::Operation::walk;
	for (int guard = 0; guard < 100; ++guard) {
		ctx.pending_floor = StashContext::IDLE;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = end;
		item.reset();
		if (!wheel.next(ctx, &item)) break;
		if (item) got.push_back(*item);
	}
}

static void test_descent_window() {
	// An announced insert at key 3 holds first_valid at or below 3 through a walk
	// to 8, though nothing is stored at 3 yet; once it lands, the next walk fires it.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		std::vector<int> got;
		{
			StashContext::InFlight in_flight(ctx, 3);
			assert(in_flight.key() == 3);
			drain(wheel, ctx, 8, got);
			assert(got.empty());
			assert(ctx.atom_first_valid_key.load() <= 3 && "walk passed an in-flight insert");
			wheel.put(ctx, in_flight.key(), std::make_shared<int>(33));
		}
		drain(wheel, ctx, 8, got);
		assert(got.size() == 1 && got[0] == 33);
	}

	// A key below the floor of a walk that already ran is filed at the floor, so
	// a later walk with the same end fires it; nothing is left behind.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		std::vector<int> got;
		wheel.add(ctx, 1, std::make_shared<int>(11));
		drain(wheel, ctx, 8, got);
		assert(got.size() == 1 && got[0] == 11);
		wheel.add(ctx, 2, std::make_shared<int>(22));   // 2 < floor (7)
		drain(wheel, ctx, 8, got);
		assert(got.size() == 2 && got[1] == 22 && "late insert below the floor was lost");
		assert(ctx.atom_first_valid_key.load() <= ctx.atom_insert_floor.load());
	}

	// A walk of several next() calls that continues from begin_key (concurrent.cc's
	// style): the snapshot taken where the walk starts must bound every call. Call
	// 1 sees an announce at 3 and returns 55; the producer then lands 3 and
	// retracts before call 2 continues from 5. The next walk must still fire 33.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		std::vector<int> got;
		Item item;
		wheel.add(ctx, 5, std::make_shared<int>(55));
		auto in_flight = std::make_unique<StashContext::InFlight>(ctx, 3);
		ctx.op = StashContext::Operation::walk;
		ctx.pending_floor = StashContext::IDLE;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = 8;
		assert(wheel.next(ctx, &item) && *item == 55);
		wheel.put(ctx, in_flight->key(), std::make_shared<int>(33));
		in_flight.reset();
		while (wheel.next(ctx, &item)) {}
		drain(wheel, ctx, 8, got);
		assert(got.size() == 1 && got[0] == 33 && "continuation walk stranded a landed insert");
	}

	// More inserts in flight than announce slots: the overflow holds first_valid
	// where it stands until they finish, then the walk advances again. A first
	// walk moves the window past 6, so these inserts take the board.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		std::vector<int> got;
		drain(wheel, ctx, 2, got);
		auto held = ctx.atom_first_valid_key.load();
		std::vector<std::unique_ptr<StashContext::InFlight>> slots;
		for (size_t i = 0; i < StashContext::ANNOUNCE_SLOTS; ++i) {
			slots.push_back(std::make_unique<StashContext::InFlight>(ctx, 6));
		}
		{
			g_overflows = g_held_overflow = g_held_far = 0;
			StashContext::InFlight overflowed(ctx, 3);
			assert(ctx.atom_announce_overflow.load() == 1);
			assert(g_overflows == 1 && "overflow not reported");
			drain(wheel, ctx, 8, got);
			assert(ctx.atom_first_valid_key.load() == held && "walk passed an overflowed insert");
			assert(g_held_overflow > 0 && g_held_far == 0 && "overflow hold not reported");
		}
		slots.clear();
		auto held_overflow = g_held_overflow;
		drain(wheel, ctx, 8, got);
		drain(wheel, ctx, 9, got);
		assert(g_overflows == 1 && g_held_overflow == held_overflow && "hold reported with nothing held");
		assert(ctx.atom_first_valid_key.load() > 3 && "first_valid stuck after the overflow drained");
		assert(got.empty());
	}

	// A far insert (key at or past the window) counts itself in its window epoch
	// instead of taking a slot. Stalled while the walk moves the window and walks
	// past its key, it holds first_valid at the window it read, and the window
	// stays put until it lands; then it fires and both move on.
	{
		Wheel wheel;
		StashContext ctx(0ULL);
		std::vector<int> got;
		drain(wheel, ctx, 8, got);
		auto w = ctx.atom_window.load();
		assert(w > 7 && w < 100);
		g_held_overflow = g_held_far = 0;
		auto far_count = [&] {
			unsigned n = 0;
			for (auto& a : ctx.announce) n += a.far[0].load() + a.far[1].load();
			return n;
		};
		auto in_flight = std::make_unique<StashContext::InFlight>(ctx, 100);
		assert(in_flight->key() == 100 && far_count() == 1 && ctx.announce[0].key.load() == StashContext::IDLE);
		drain(wheel, ctx, 90, got);
		auto w2 = ctx.atom_window.load();
		assert(w2 > w && ctx.far_pinned && ctx.atom_first_valid_key.load() <= w);
		assert(g_held_far > 0 && g_held_overflow == 0 && "far pin hold not reported");
		drain(wheel, ctx, 200, got);
		assert(ctx.atom_window.load() == w2 && "window moved over a live epoch");
		assert(ctx.atom_first_valid_key.load() <= w && "walk passed a stalled far insert");
		wheel.put(ctx, in_flight->key(), std::make_shared<int>(100));
		in_flight.reset();
		drain(wheel, ctx, 200, got);
		assert(got.size() == 1 && got[0] == 100 && "stalled far insert was lost");
		drain(wheel, ctx, 300, got);
		assert(ctx.atom_window.load() > w2 && ctx.atom_first_valid_key.load() > 100);
	}

	std::printf("stash OK: in-flight inserts pin first_valid; late inserts still fire\n");
}

// An add that fails to allocate (any site: a node, a chunk, a child level, the
// value box) stores nothing and leaves no reserved hole: the walk still drains
// and advances first_valid past the key, and a later add one span out is not
// refused. Both a fresh leaf and a full leaf chunk (the add grows the chain).
static void test_alloc_failure() {
	for (int prefill : {0, 4}) {
		for (int fail_at = 1;; ++fail_at) {
			Wheel wheel;
			StashContext ctx(0ULL);
			std::vector<int> got;
			for (int i = 0; i < prefill; ++i) {
				wheel.add(ctx, 3, std::make_shared<int>(30 + i));
			}
			auto value = std::make_shared<int>(99);
			bool threw = false;
			g_fail_at = fail_at;
			try {
				wheel.add(ctx, 3, value);
			} catch (const std::bad_alloc&) {
				threw = true;
			}
			g_fail_at = 0;
			if (!threw) {
				assert(fail_at > 1 && "the add allocated nothing");
				break;
			}
			assert(value.use_count() == 1 && "a failed add kept the value");
			for (auto& a : ctx.announce) {
				assert(a.key.load() == StashContext::IDLE && "a failed add left its announce");
			}
			drain(wheel, ctx, 8, got);
			assert(static_cast<int>(got.size()) == prefill && "a failed add lost or stored a value");
			assert(ctx.atom_first_valid_key.load() > 3 && "a failed add left a hole pinning first_valid");
			drain(wheel, ctx, 9, got);
			bool added = wheel.add(ctx, 3 + 8 + 4, std::make_shared<int>(7));
			assert(added && "first_valid stuck: an add one span out was refused");
			drain(wheel, ctx, 16, got);
			assert(got.size() == static_cast<size_t>(prefill) + 1 && got.back() == 7);
			(void)added;
		}
	}
	std::printf("stash OK: a failed allocation in add leaves no hole; first_valid advances\n");
}

int main() {
	test_smoke();
	test_low_key_no_underflow();
	test_high_key_span_guard();
	test_descent_window();
	test_alloc_failure();
	std::printf("all stash tests passed\n");
	return 0;
}
