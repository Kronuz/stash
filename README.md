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
- **`StashSlots<T, Size, Div, Mod>`** — a keyed level: a key
  maps to slot `(key / Div) % Mod` (`stash.h:240`), and `T` is itself a
  `Stash`, so levels **nest** to give multiple time resolutions. `next()`
  walks a key window in one of three modes — walk (consume), peep (look-ahead),
  clean (GC).
- **`StashValues<T, Size>`** — the leaf: an append-only list with
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

using Item  = std::shared_ptr<int>;              // values must be pointer-like
using Leaf  = StashValues<Item, 4>;
using Wheel = StashSlots<Leaf, 8, /*Div*/1, /*Mod*/8>;

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

### `StashSlots<T, Size, Div, Mod>`

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

### `StashValues<T, Size>`

The append-only leaf level (`stash.h:371`).

- `put(ctx, key, args...)` — appends, bumping the atomic write cursor
  `atom_end` (`stash.h:462`). The leaf ignores `key`; ordering comes from the
  slot levels above it.
- `next(ctx, value_ptr, limit_key)` — reads from `walk_cur` (or `clean_cur` for
  clean), returns the first non-empty pointer-like value, and consumes it
  unless the op is `peep` (`stash.h:391`).

## Tracing and colors

`stash.h` does not hard-code its own tracing. It instruments itself through four
hooks that are no-ops by default, so out of the box the data structure builds
with zero dependency on any logging or color header and tracing costs nothing at
runtime:

- `L_STASH(fmt, args...)` — the main trace macro on the insert and walk paths.
- `L_DEBUG_HOOK(label, fmt, args...)` — per-iteration loop trace; takes a label
  as its first argument.
- `L_EXC(msg)` — used to swallow exceptions in the manual destructor.
- `STASH_OP_COLOR(op)` — returns a C string (an ANSI escape) used to tint a
  trace line per `StashContext::Operation`. `StashContext::_col()` returns this
  (`stash.h:117`). It is `""` by default.

The bundled `stash_trace.h` supplies the no-op defaults, each one
`#ifndef`-guarded so you can override any subset and let the rest fall back.
There are two ways to plug in real tracing:

1. Point `STASH_TRACE_HEADER` at a header that defines the hooks. `stash.h`
   includes it in place of `stash_trace.h` (`stash.h:34`):

   ```sh
   c++ -std=c++20 -DSTASH_TRACE_HEADER='"my_trace.h"' ...
   ```

2. Define the macros directly before including `stash.h`.

Nothing is required by default. A complete, runnable override lives in
[`examples/colored_trace/`](examples/colored_trace/): it defines the color
constants, `STASH_OP_COLOR`, and the `L_*` macros, rendering with `std::format`
to produce colored, fully-formatted trace lines. That example needs C++20 for
`std::format` / `std::vformat`, even though `stash.h` itself only requires C++17.
It is how a consumer such as Xapiand recovers the colored debug tracing it relies
on without editing `stash.h`.

## Build & test

Header-only (`stash.h` plus `stash_trace.h`, a tiny set of no-op trace stubs).
To run the smoke test:

```sh
c++ -std=c++17 -I. test/test.cc -o test/test && ./test/test
# or: cmake -B build && cmake --build build && ctest --test-dir build
```

The test prints `stash OK: walked 3 values in key order: 11 22 33` and exits 0.

## Concurrency model & invariants

`stash` is lock-free on the producer side and single-consumer on the read/reclaim
side. Many threads may `add`/`put` concurrently; exactly **one** thread runs the
three read-side operations, which must not run concurrently with each other:

- **`walk`** moves the present forward: it drains and fires everything due, frees
  the consumed leaf *values*, and advances `first_valid`. It does **not** free
  structure.
- **`peep`** is a read-only lookahead (find the soonest entry, e.g. to size a
  sleep). It advances no cursor and frees nothing.
- **`clean`** reclaims *structure*: it drops a slot's whole subtree once that
  slot's window is entirely behind a safe cutoff. **`clean` is required** — with
  no `clean` the structure grows without bound (each reused leaf's append cursor
  never resets). It is a separate pass, by design, because it may only release
  what is provably past the consumer and unreachable by producers.

Correctness rests on these invariants, all of which the Xapiand scheduler
satisfies:

1. **One consumer.** A single thread runs `walk` then `clean` sequentially. Two
   walkers, or `clean` concurrent with `walk`, is undefined behavior.
2. **Producers never write the past.** Keys are clamped to `>= now`, so no
   producer targets a slot the consumer has passed — except transiently at a slot
   boundary (a producer that latched a slot just before the clock crossed it).
3. **`clean` trails by a margin longer than any in-flight `add`.** The cutoff
   (`now - margin` in the scheduler) covers that boundary case: a slot a margin in
   the past is being touched by no one, so its subtree is dropped with no locks or
   checks.
4. **No scheduling near the wrap horizon.** Keys stay below the wheel span; the
   overflow guard drops anything beyond it. Scheduling within a margin of the full
   horizon would alias a future write onto a slot being reclaimed.

Inside this envelope — one consumer, bounded rates, a margin longer than an
operation, no near-horizon scheduling — `stash` is correct, and that is exactly
how the scheduler uses it. **Outside it, this is not a general-purpose MPMC
structure.** A synthetic hammer (many producers at extreme rates, a margin shorter
than an operation, or scheduling at the horizon) can expose a small rate of lost
entries via a boundary use-after-free, near-horizon aliasing, or a bounds-ordering
strand under heavy contention. Making reclamation *certain* under arbitrary
concurrency needs real safe-memory-reclamation (epoch / hazard pointers) plus a
linearized insert/walk — a planned redesign, not a property of the current
margin-based `clean`.

`test/longevity.cc` demonstrates the reclamation (unbounded growth without
`clean`, bounded with it); `test/concurrent.cc` exercises the envelope under
producer/consumer contention with ASan/TSan.

### Hardening knobs

Two of the three envelope dangers have cheap, exact fixes; the third (the
bounds-ordering strand) does not, short of the planned redesign.

- **R1 — boundary use-after-free.** Only possible if a producer stalls *longer
  than the clean margin* mid-`add`. The clean cutoff (`now - margin`, the
  consumer's choice) is the safety buffer: a bigger margin tolerates longer
  stalls at the cost of holding more not-yet-reclaimed structure. The scheduler's
  one-minute margin already makes this astronomically unlikely; raise it if you
  must tolerate longer pauses. Not a code change here — it is the consumer's
  `clean` cutoff.
- **R2 — near-horizon aliasing.** Set `StashContext::horizon_margin` (default 0)
  to a keep-out zone `>=` the clean margin: `add()` then rejects keys within that
  much of the horizon, so a near-horizon insert can never alias onto a slot being
  reclaimed one period below. The operational cost is that you can schedule up to
  `span - horizon_margin` instead of the full `span` (e.g. ~24h minus a minute).
- **The strand** has no cheap knob: it is the walk observing an insert mid-flight
  (a leaf slot reserved with `atom_end++` but not yet written, or a bound bumped
  after the value). Closing it needs a linearized insert/walk (the redesign).

## Notes & caveats

- **Value type must be pointer-like** (bool-testable and dereferenceable, e.g.
  `std::shared_ptr<T>`). The walk tests `*ptr && **ptr`. Don't store a value that
  is "zero/empty" as a live entry; it reads as absent.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where it is the
engine behind the scheduler thread (which drives the async logger and every
debounced background job — fsync, commit, replication triggers). For the full
timer-wheel / scheduler design, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
