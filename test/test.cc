// Smoke test for the standalone stash library.
//
// Drives the lock-free hierarchical slot store the way SchedulerQueue does: a
// keyed wheel (StashSlots) over an append-only leaf (StashValues), with a
// deterministic integer "clock". Values are pointer-like (shared_ptr), as the
// container requires.
//
// Build: c++ -std=c++17 -I.. test.cc -o test && ./test
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
#include "stash.h"

// Deterministic key source (the vestigial _CurrentKey template parameter).
static unsigned long long now() { return 0; }

int main() {
	using Item  = std::shared_ptr<int>;
	using Leaf  = StashValues<Item, 4, &now>;
	using Wheel = StashSlots<Leaf, 8, &now, /*Div*/1, /*Mod*/8, /*Ring*/true>;

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
	return 0;
}
