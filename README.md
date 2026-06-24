# stash

A lock-free, hierarchical **slot store** — the data structure underneath a
multi-resolution timer wheel.

## What it is

`stash` is a header-only C++17 container for keyed, time-ordered values.
Producers insert values keyed by an unsigned integer using atomic
compare-and-swap, so they never take a lock. A single consumer walks a key
window in order, optionally consuming the entries or garbage-collecting cleared
slots as it goes. It is the storage primitive a scheduler or timer wheel sits
on top of, not a scheduler itself.

## How it works

Three composable templates build up from a primitive to a full keyed wheel:

- **`Stash<T, Size>`** — the primitive: a chunked, lazily-grown array of
  `std::atomic<T*>`. Slots beyond `Size` spill into a linked `Data` node;
  chunks and nodes are published with `compare_exchange`, so concurrent
  producers never block (`stash.h:154`).
- **`StashSlots<T, Size, CurrentKey, Div, Mod, Ring>`** — a keyed level: a key
  maps to slot `(key / Div) % Mod` (`stash.h:240`), and `T` is itself a
  `Stash`, so levels **nest** to give multiple time resolutions. `next()`
  walks a key window in one of three modes — walk (consume), peep (look-ahead),
  clean (GC).
- **`StashValues<T, Size, CurrentKey>`** — the leaf: an append-only list with
  an atomic write cursor (`atom_end`) and separate walk/clean read cursors
  (`stash.h:375`).

Nesting a few `StashSlots` over a `StashValues` gives a timer wheel. Xapiand's
scheduler stacks `StashValues + 4 StashSlots` into a 24-hour horizon at
millisecond granularity: `50x1ms -> 10x50ms -> 36x500ms -> 4800x18s`. See
`test/test.cc` for a minimal single-level wheel.

## When to use it / when not

Use it when you are building something that needs to bucket values by an
integer key (usually a clock tick) and drain them in key order with low
contention on the insert path: a scheduler queue, a timer wheel, a debounce
table. It is a low-level building block. The expected driver is a scheduler
thread or timer wheel that owns the clock and calls `add` on the producer side
and `next` on a single consumer thread.

Do not reach for it as a general-purpose map or queue. The value type must be
pointer-like, a "zero" value reads as absent, and the walk side is
single-consumer. If you want concurrent consumers, a generic associative
container, or arbitrary value types, this is the wrong tool.

## Install

Header-only. Drop `stash.h` and `stash_trace.h` on your include path and:

```cpp
#include "stash.h"
```

Requires C++17. With CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  stash
  GIT_REPOSITORY https://github.com/Kronuz/stash.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(stash)

target_link_libraries(your_target PRIVATE stash)
```

The `stash` target is an `INTERFACE` library that adds the include path and
requests `cxx_std_17` (`CMakeLists.txt:4`).

## Usage

`stash` is a low-level building block; the intended consumer is a scheduler
that keys by a clock. A minimal single-level wheel, taken from `test/test.cc`:

```cpp
#include "stash.h"

// Deterministic key source (the vestigial _CurrentKey template parameter).
static unsigned long long now() { return 0; }

using Item  = std::shared_ptr<int>;              // values must be pointer-like
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
// got == {11, 22, 33}
```

The key points: `add` places a value at an absolute integer key; the wheel maps
that key down to a slot at each level. The walk reads `atom_first_valid_key`
into `ctx.begin_key` each iteration so it skips empty ranges, sets `ctx.end_key`
as the upper bound of the window, and calls `next` until it returns `false`.
Because `op` is `walk`, each returned slot is consumed (cleared) as it is read.

For the full timer-wheel / scheduler design this was lifted from, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).

## API reference

### `StashContext`

Carries the state for a single traversal (`stash.h:47`).

- `Operation op` — `walk`, `peep`, or `clean`. Walk reads and consumes; peep
  reads without consuming; clean only reclaims cleared slots.
- `begin_key` / `end_key` — the half-open key window `[begin_key, end_key)` the
  traversal covers. `end_key == 0` means "no upper bound from the window."
- `atom_first_valid_key` / `atom_last_valid_key` — atomic bounds on where live
  data is. `add` widens them; the walk advances `atom_first_valid_key` so the
  next pass can skip ranges already drained.
- `check(key, limit_key)` — the loop predicate: stops when `key` reaches
  `end_key`, `limit_key`, or passes `atom_last_valid_key` (`stash.h:76`).

### `Stash<T, Size>`

The chunked atomic-pointer array primitive (`stash.h:113`).

- `get(pptr_atom_ptr, slot, spawn)` — resolves `slot` to the address of an
  `std::atomic<T*>`. With `spawn == true` it lazily allocates chunks and linked
  nodes via CAS; with `spawn == false` it reports `StashEmpty`, `ChunkEmpty`,
  or `StashShort` instead of allocating (`stash.h:209`).

### `StashSlots<T, Size, CurrentKey, Div, Mod, Ring>`

A keyed level over a `Stash` whose element type `T` is itself a `Stash`
(another `StashSlots` or a `StashValues`) (`stash.h:220`).

- `put(ctx, key, args...)` — maps `key` to slot `(key / Div) % Mod`, lazily
  creates the child level, and recurses (`stash.h:332`).
- `add(ctx, key, args...)` — `put` plus bound maintenance; throws
  `std::out_of_range("stash overflow")` if `key` is beyond the wheel span
  `[first_valid_key, first_valid_key + Div*Mod)` (`stash.h:354`).
- `next(ctx, value_ptr [, limit_key])` — walks the key window in `ctx.op` mode,
  recursing into the child level, returning the first matching value
  (`stash.h:250`).

### `StashValues<T, Size, CurrentKey>`

The append-only leaf level (`stash.h:371`).

- `put(ctx, key, args...)` — appends, bumping the atomic write cursor
  `atom_end` (`stash.h:462`). The leaf ignores `key`; ordering comes from the
  slot levels above it.
- `next(ctx, value_ptr, limit_key)` — reads from `walk_cur` (or `clean_cur` for
  clean), returns the first non-empty pointer-like value, and consumes it
  unless the op is `peep` (`stash.h:391`).

## Build & test

Header-only (`stash.h` plus `stash_trace.h`, a tiny set of no-op trace stubs).
To run the smoke test:

```sh
c++ -std=c++17 -I. test/test.cc -o test/test && ./test/test
# or: cmake -B build && cmake --build build && ctest --test-dir build
```

The test prints `stash OK: walked 3 values in key order: 11 22 33` and exits 0.

## Notes & caveats

- **Value type must be pointer-like** (bool-testable and dereferenceable, e.g.
  `std::shared_ptr<T>`). The walk tests `*ptr && **ptr` (`stash.h:437`). Don't
  store a value that is "zero/empty" as a live entry; it reads as absent.
- **Single-consumer walk.** Many producers can `add`/`put` concurrently, but
  the walk/peep/clean side mutates non-atomic cursors (`walk_cur`,
  `clean_cur`) and is not safe for concurrent walkers.
- **Standalone changes from the Xapiand original:** the `#include "log.h"` was
  replaced by a local `stash_trace.h` that defines empty `L_*` trace macros
  (define your own before including `stash.h` to plug in tracing), and the
  debug-only color helper `StashContext::_col()` now returns `""` to drop the
  ANSI-color dependency (`stash.h:107`).
- **Vestigial template parameters:** `CurrentKey` (the `&now` function pointer)
  and `Ring` are declared but unused in the bodies. They are kept for
  source-compatibility with the original and can be dropped in a future
  cleanup.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where it is the
engine behind the scheduler thread (which drives the async logger and every
debounced background job — fsync, commit, replication triggers). For the full
timer-wheel / scheduler design, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
