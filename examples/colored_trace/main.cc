// Demonstrates that stash's tracing/coloring is fully recoverable by a consumer.
// This build injects a trace header (trace.h) that turns stash's no-op hooks
// into colored, std::format-rendered trace output. With no STASH_TRACE_HEADER
// (the default), the exact same stash.h produces no trace and no color.
//
// Build & run:
//   c++ -std=c++20 -I. -I../.. -DSTASH_TRACE_HEADER='"trace.h"' main.cc -o demo && ./demo
#include <cstdio>
#include <memory>

#include "stash.h"

int main() {
	std::puts("=== stash with a colored trace header injected ===");

	using Item = std::shared_ptr<int>;
	using Leaf = StashValues<Item, 4>;
	using Wheel = StashSlots<Leaf, 8, /*Div*/1, /*Mod*/8>;

	Wheel wheel;
	StashContext ctx(0ULL);
	wheel.add(ctx, 1, std::make_shared<int>(11));
	wheel.add(ctx, 2, std::make_shared<int>(22));

	ctx.op = StashContext::Operation::walk;
	Item item;
	for (int guard = 0; guard < 50; ++guard) {
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = 5;
		item.reset();
		if (!wheel.next(ctx, &item)) break;
	}

	std::puts("=== done (the colored lines above came through stash's trace hooks) ===");
	return 0;
}
