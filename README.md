# stash

[![CI](https://github.com/Kronuz/stash/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/stash/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

A lock-free, hierarchical slot store for C++17: many threads insert values by
integer key without locks, and one consumer drains them in key order. It is the
storage under a multi-resolution timer wheel, for anyone building a scheduler,
debouncer, or timer queue.

![stash: producers insert through nested slot levels; one consumer drains in key order](docs/stash.svg)

## ✨ Highlights

- **Producers never block each other.** Every insert is an atomic fetch-add or a
  compare-and-swap; there is no lock to queue behind.
- **Flat tail latency under contention.** With 14 threads inserting, p99 stays at
  1.4–1.8 µs, where a locked heap and Asio's timers reach 65 µs to over 1 ms
  ([bench/RESULTS.md](bench/RESULTS.md), Apple M4 Pro).
- **Memory follows what is live.** Levels and chunks are allocated on first use,
  so a 24-hour wheel at 1 ms resolution costs only the slots that hold something.
- **Answers "what is due now?"**, which a lock-free queue cannot.
- **Header-only, no dependencies.** Tracing hooks are no-ops unless you inject
  your own.

## 📦 Installation

Requires a C++17 compiler and, for the CMake route, CMake 3.14 or newer. CI
builds it with GCC and Clang on Linux (x86_64, arm64), macOS (arm64, x86_64), and
FreeBSD.

With CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(stash
  GIT_REPOSITORY https://github.com/Kronuz/stash.git
  GIT_TAG        main)
FetchContent_MakeAvailable(stash)

target_link_libraries(my_app PRIVATE stash::stash)
```

Or copy `stash.h` and `stash_trace.h` onto your include path.

## 🚀 Quick start

A one-level wheel over a leaf: insert at keys 1–3, then walk everything due
before key 6. This is the smoke test in [`test/test.cc`](test/test.cc).

```cpp
#include "stash.h"

#include <cstdio>
#include <memory>

using Item  = std::shared_ptr<int>;                  // values must be pointer-like
using Leaf  = StashValues<Item, 4>;
using Wheel = StashSlots<Leaf, 8, /*Div*/ 1, /*Mod*/ 8>;

int main() {
	Wheel wheel;
	StashContext ctx(0);
	wheel.add(ctx, 1, std::make_shared<int>(11));
	wheel.add(ctx, 2, std::make_shared<int>(22));
	wheel.add(ctx, 3, std::make_shared<int>(33));

	Item item;
	for (;;) {
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = 6;
		item.reset();
		if (!wheel.next(ctx, &item)) { break; }
		if (item) { std::printf("%d ", *item); }   // prints 11 22 33
	}
}
```

[`examples/demo.cc`](examples/demo.cc) tours the rest: nested levels, `peep`,
the horizon guard, and a producer/consumer run.

## 🎯 When to use it

- ✅ Bucketing values by a clock tick and draining them in order, with many
  producers: scheduler queues, timer wheels, debounce tables.
- ❌ A general map or queue, more than one consumer, or values that are not
  pointer-like (a null or "zero" value reads as absent).

## 📚 Documentation

- [docs/API.md](docs/API.md): every type and member, and the tracing hooks.
- [ARCHITECTURE.md](ARCHITECTURE.md): the three templates, traversal, the
  concurrency model and reclamation, and the cost model.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps, for anyone
  changing the code.
- [bench/](bench/README.md): the benchmarks and [their results](bench/RESULTS.md).
- [examples/colored_trace/](examples/colored_trace/README.md): plugging in
  colored tracing without editing `stash.h`.
- [Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md):
  the scheduler built on it.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/stash_demo
```

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first: it lists
the invariants a change must keep. CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE). Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand).
