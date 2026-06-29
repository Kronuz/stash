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

## Cost

For `N` live values and `P` concurrent producers, with keys spread across the
future (the workload it is built for):

- **Insert** is `O(1)` amortized and lock-free: a `Div`/`Mod` descent through a
  constant number of levels, then an append to the leaf. The only shared writes
  are the valid-key bounds and the leaf's append cursor, both non-blocking, so no
  producer ever waits on another. Convergent keys (everything on one instant)
  degrade the leaf append toward `O(b)` for `b` values in that one bucket.
- **Drain** (`walk`) is `O(1)` amortized per fired value: the walk follows the
  valid-key bounds and skips the empty stretches, and a per-leaf cursor keeps
  sequential reads from re-walking the chunk chain.
- **Memory** is `O(N)` plus the sparse path nodes actually touched. It is **not**
  `O(range / resolution)` like a flat wheel: a 24-hour, 1 ms wheel would be 86 M
  buckets preallocated; `stash` builds only the slots that hold something.

How it compares, fairly:

| | insert under `P` producers | memory | answers "due now?" |
| --- | --- | --- | --- |
| **stash** | `O(1)` amortized, lock-free | `O(N)`, sparse | yes (slot-quantized) |
| heap + mutex | `O(P log N)`, serialized on one lock | `O(N)` | yes (exact order) |
| flat timer wheel | `O(1)` | `O(range/res)` preallocated | yes (slot-quantized) |
| lock-free queue | `O(1)`, scales ~linearly | `O(N)` | **no** |

The asymptotics are not the headline; the columns are. Against a heap the win is
that producers never serialize behind a lock, so tail latency stays flat under
contention. Against a flat wheel it is `O(N)` memory instead of a preallocated
continent. Against a lock-free queue, `stash` can answer "what is due now" and the
queue cannot. It owns one cell of that table: many producers, one consumer, drain
in time order, memory proportional to what is live.

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
  slot is entirely below `first_valid`, the walk's live low-water mark.
  **`clean` is required** (without it the structure grows without bound, since
  each reused leaf's append cursor never resets).

`clean` is **walk-based** ("clean as walking"): its cutoff is `first_valid`, not
a wall-clock time. That is safe because `first_valid` is a true reclamation
boundary, held by two mechanisms in the insert/walk protocol:

- A leaf with an in-flight insert (a slot reserved with `atom_end++` but not yet
  written) reports itself *pending*, and the walk caps `first_valid` at that
  leaf's key (`pending_floor`). The low-water mark never crosses a leaf a producer
  is still filling.
- A late insert (a key that has just gone past) lowers `first_valid` to its own
  key, before the value is published, so `clean` cannot reclaim under it.

Everything strictly below `first_valid` has therefore been walked and is touched
by no producer, so its subtree drops with no locks, no checks, and no time margin.

Correctness rests on:

1. **One consumer.** A single thread runs `walk` then `clean` sequentially. Two
   walkers, or `clean` concurrent with `walk`, is undefined behavior.
2. **The sentinel frontier.** Each slot encodes its own state: a reserved-but-
   unfilled slot holds a sentinel, a filled slot a value, a drained slot null. A
   chunk is sentinel-initialized before it is published, so the walk parks on the
   first sentinel and never steps past it (it never skips a reserved slot), and
   `pending_floor` keeps reclamation from crossing a pending leaf, so `clean` never
   frees under an in-flight insert. (This replaced the earlier `atom_ready` COMMIT
   loop; see `STRAND_FIX.md`.)
3. **Insert latency below task lead time.** The bound and the `first_valid`
   lowering happen *during* `add`, so a task that becomes due in the brief window
   *before* its insert publishes (the descent window) can be lost. For any real
   schedule (leads of milliseconds and up against insert latency of microseconds)
   this cannot happen; it surfaces only at a sub-millisecond leading edge, or under
   extreme oversubscription where a preempted producer's effective latency exceeds
   the lead. It is bounded loss, never a use-after-free.
4. **A span the consumer can keep up with.** The wheel reuses each physical slot
   every `Div*Mod` (one revolution). The single consumer must finish a `clean`
   pass within one revolution. If it falls a full revolution behind (span far too
   small for the offered load, or the consumer starved of CPU), a producer reusing
   a slot aliases the previous wrap's live, not-yet-cleaned subtree while `clean`
   frees it, which is a use-after-free. Size the span well above one clean pass.
   This is standard for any hierarchical timer wheel; `horizon_margin` (below)
   guards the adjacent near-horizon case.

Inside this envelope (one consumer; insert latency below lead time; the span sized
so `clean` is not lapped) `stash` reclaims with **exact accounting and no time
margin**: every entry fires exactly once, memory stays bounded, and there are no
data races or use-after-free (ASan/TSan clean), including under thread
oversubscription. `test/longevity.cc` demonstrates the reclamation (unbounded
growth without `clean`, bounded with it); `test/concurrent.cc` proves the
accounting under heavy producer/consumer contention.

This replaced an earlier **margin-based** `clean` (cutoff `now - margin`), which
bought a wall-clock quiescence window for safety but, under load, reclaimed
completed-but-overdue tasks. Walk-based `clean` removes both the loss and the
margin, and the strand fix is what makes `first_valid` trustworthy enough to do so.

### Optional: near-horizon scheduling (`horizon_margin`)

`StashContext::horizon_margin` (default 0, off) reserves a keep-out zone at the
wrap horizon: `add()` rejects keys within that much of the span, so a near-horizon
insert can never alias (via the per-level modulus) onto a slot one period below.
Leave it 0 unless you schedule within one period of the full span (for example
~24h out on a ~24h wheel); the cost is that you can then schedule up to
`span - horizon_margin` instead of the full `span`.

## Notes & caveats

- **Value type must be pointer-like** (bool-testable and dereferenceable, e.g.
  `std::shared_ptr<T>`). The walk tests `*ptr && **ptr`. Don't store a value that
  is "zero/empty" as a live entry; it reads as absent.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour of the data structure
itself. A top-level CMake build produces it next to the test:

```sh
cmake -B build && cmake --build build && ./build/stash_demo
```

It builds a two-level wheel over a leaf and walks through the core moves: how a
key descends to a leaf slot via `(key / Div) % Mod` at each level; `add`ing
values at scattered offsets (including two at the same key, kept in insertion
order by the append-only leaf); `peep`ing the soonest entry without consuming
it; `walk`ing a key window so it drains in key order and skips the empty
stretches; the horizon guard rejecting a key one full span out; and a small
deterministic producer/consumer that shows the lock-free insert path (many
`add`s, one consumer draining every value exactly once). The output is the same
every run.

[`examples/colored_trace/`](examples/colored_trace/) is the other example: it
injects a trace header that turns `stash`'s no-op tracing hooks into colored,
`std::format`-rendered trace lines, showing how a consumer recovers full debug
tracing without editing `stash.h`. See its [README](examples/colored_trace/README.md).

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where it is the
engine behind the scheduler thread (which drives the async logger and every
debounced background job — fsync, commit, replication triggers). For the full
timer-wheel / scheduler design, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
