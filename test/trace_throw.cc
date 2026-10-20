// A trace hook that throws std::bad_alloc inside StashValues::put (a trace build
// whose L_STASH allocates) must not leave a claimed slot unfilled: add returns
// no_memory with nothing claimed, and the walk still advances first_valid.
#include <cassert>
#include <cstdio>
#include <memory>
#include <new>
#include <string>

static const std::string PURPLE, LIGHT_PURPLE, CLEAR_COLOR, CYAN, SADDLE_BROWN, FOREST_GREEN, LIGHT_RED;
static bool g_arm = false;
template <typename... A>
static void throwing_trace(const std::string& fmt, A&&...) {
	if (g_arm && fmt.find("StashValues::") == 0) {
		g_arm = false;
		throw std::bad_alloc();
	}
}
#define L_STASH(fmt, ...) throwing_trace(fmt)
#include "stash.h"

using Item = std::shared_ptr<int>;
using Wheel = StashSlots<StashValues<Item, 4>, 8, 1, 8>;

int main() {
	Wheel wheel;
	StashContext ctx(0ULL);
	g_arm = true;
	auto value = std::make_shared<int>(1);
	auto result = wheel.add(ctx, 3, value);
	assert(result == stash_result::no_memory && value.use_count() == 1 && "a throwing trace stored the value");
	assert(wheel.add(ctx, 3, std::make_shared<int>(2)) == stash_result::placed);
	Item out;
	int fired = 0;
	for (unsigned long long end = 4; end <= 12; ++end) {
		ctx.op = StashContext::Operation::walk;
		ctx.pending_floor = StashContext::IDLE;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = end;
		while (wheel.next(ctx, &out)) {
			if (out) ++fired;
			out.reset();
		}
	}
	assert(fired == 1 && "the placed value did not fire");
	assert(ctx.atom_first_valid_key.load() > 3 && "a throwing trace left a hole pinning first_valid");
	(void)result;
	std::printf("stash OK: a throwing trace in add leaves no hole\n");
	return 0;
}
