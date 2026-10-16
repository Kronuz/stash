# Architecture

This document describes the internal design of `stash`: how the three templates
compose, the concurrency model, the traversal machinery, and the trade-offs
baked into the data structure. Code references name symbols in `stash.h`.

## Overview

`stash` is a lock-free hierarchical slot store. The job it does is bucket
pointer-like values under an unsigned integer key and drain them in key order
with cheap, lock-free inserts. It is the storage layer a multi-resolution timer
wheel sits on.

The design is three templates layered on top of each other:

1. `Stash<T, Size>` — the primitive. A chunked array of `std::atomic<T*>` that
   grows lazily and never blocks producers.
2. `StashSlots<T, Size, Div, Mod>` — a keyed level built on a
   `Stash`. Maps a key to a slot, and because its element type `T` is itself a
   `Stash`, levels nest to form a multi-resolution wheel.
3. `StashValues<T, Size>` — the leaf, also built on a `Stash`. An
   append-only list of values with a write cursor and read cursors.

A concrete wheel is a type like
`StashSlots<StashSlots<...StashValues...>>`. Xapiand stacks
`StashValues + 4 StashSlots` into a 24-hour wheel:
`50x1ms -> 10x50ms -> 36x500ms -> 4800x18s`. The smoke test in `test/test.cc`
collapses that to a single `StashSlots` level over one `StashValues` leaf.

`StashContext` is the cross-cutting state object threaded through every
operation. It is not part of the layering; it carries the current operation,
the key window, and the atomic key bounds.

## The primitive: `Stash<T, Size>` and its `Data` node

`Stash<T, Size>` owns a single `Data data` member and forwards `get` to it. All the real work is
in the nested `Data` class.

### The chunked atomic-pointer array

A `Data` node holds:

- `std::atomic<Chunks*> atom_chunk`, where
  `Chunks = std::array<std::atomic<_Tp*>, _Size>`. This is one
  chunk: a fixed-size array of `Size` atomic element pointers.
- `std::atomic<Data*> atom_next` — a pointer to the next node in a linked list
  of nodes.

So the logical array is a linked list of `Data` nodes, each holding (at most)
one chunk of `Size` slots. Logical slot `N` lives in node `N / Size` at offset
`N % Size`. Both the chunk and the next node start as `nullptr` and are
allocated only when a slot in that range is first touched.

### CAS-based lazy growth in `Data::get`

`Data::get(pptr_atom_ptr, slot, spawn)` resolves a logical slot
to the address of its `std::atomic<_Tp*>`, allocating storage along the way when
`spawn` is true. It walks the structure in two phases.

First, the early-out: if `spawn` is false and the node has neither a next node
nor a chunk, it returns `StashState::StashEmpty`. This is how a
read-side walk cheaply discovers an untouched subtree.

Second, node traversal. If `slot >= _Size`, the target is in a later node, so it
computes `chunk_num = slot / _Size` and reduces `slot` to `slot % _Size`, then walks `chunk_num` hops down the `atom_next` chain. At each hop:

- If the next node does not exist and `spawn` is false, it returns
  `StashState::StashShort`: the read side has run off the end of
  the materialized list.
- If the next node does not exist and `spawn` is true, it allocates a fresh
  `Data` and publishes it with
  `atom_next.compare_exchange_strong(next, tmp)`. If the CAS
  loses to a racing producer, it deletes its own `tmp` and adopts the winner's
  node. Either way it advances to a valid `next`.

Third, chunk allocation. Once on the right node, it loads `atom_chunk`. If the
chunk is missing and `spawn` is false, it returns `StashState::ChunkEmpty`. If `spawn` is true, it allocates a chunk (null slots, or
sentinels for the leaf) and publishes it with `atom_chunk.compare_exchange_strong`, again deleting its own allocation if it loses the race.

Finally it returns the address of `(*chunk)[slot]` and
`StashState::Ok`. The caller then operates on that single `std::atomic<_Tp*>`.

The pattern repeats at every level of the structure: never lock, always
allocate-then-CAS-then-reconcile. A losing producer never corrupts state; it
just discards its speculative allocation and continues with whatever the winner
installed.

## The keyed level: `StashSlots`

`StashSlots<_Tp, _Size, _Div, _Mod>` derives
from `Stash<_Tp, _Size>`, so it has the chunked array underneath. Its job is to
turn a key into a slot index and recurse into the child level stored there.

### Key-to-slot mapping and nesting

The slot for a key is `(key / _Div) % _Mod` (`get_slot`). `_Div`
is the resolution of this level (how many key units map to one slot), and `_Mod`
is the number of slots. A few helpers derive key boundaries from this:

- `get_base_key(key) = (key / _Div) * _Div` — the key at the start of the slot
  `key` falls in.
- `get_inc_base_key(key) = get_base_key(key) + _Div` — the start of the next
  slot.
- `get_dec_base_key(key) = get_base_key(key) - _Div` — the start of the previous
  slot.
- `get_end_base_key(key) = get_base_key(key) + _Div * _Mod` — one wheel span
  past the slot's base, used as the overflow boundary.

Because the element type `_Tp` of a `StashSlots` is itself a `Stash` (a finer
`StashSlots`, or the `StashValues` leaf), the levels nest. The outermost level
divides the key space coarsely; each nested level refines the residual. With the
Xapiand stack, the top level buckets into 18-second slots, and the chain down
narrows resolution to 1 ms at the leaf. The `_Div` values multiply down the
chain so each level's span equals the next level's slot width.

### Inserting: `put` and `add`

`put(ctx, key, args...)` computes the slot, calls
`Stash_T::get(&ptr_atom_ptr, slot, true)` to materialize storage, then ensures a
child level exists in that slot: if the slot's pointer is null it allocates a
`new _Tp()` and installs it with `compare_exchange_strong`, deleting its own
allocation on a lost race. Then it forwards `put` to the child. The recursion bottoms out at `StashValues::put`.

`add(ctx, key, args...)` is `put` with bounds checking and
bound maintenance. Before inserting it checks
`key >= get_end_base_key(ctx.atom_first_valid_key.load())` and throws
`std::out_of_range("stash overflow")` if the key is more than one wheel span
past the first valid key. After inserting, it widens the context
bounds with CAS loops: it lowers `atom_first_valid_key` toward `key` and raises
`atom_last_valid_key` toward `key`. These bounds are what later
let the walk skip empty key ranges.

## The leaf: `StashValues`

`StashValues<_Tp, _Size>` also derives from
`Stash<_Tp, _Size>`, but it ignores the key entirely. It is an append-only log.

It adds a producer frontier and the consumer's cursors:

- `std::atomic_size_t atom_end` — the reserve frontier: the next slot a producer
  claims.
- `size_t walk_cur`, `size_t clean_cur` — the walk's and clean's positions
  (non-atomic; one consumer), with `walk_cursor` and `clean_cursor` caching where
  each resumes in the chunk chain.

Each leaf slot is in one of three states, which the walk reads directly:
**reserved** (it holds the sentinel `stash_reserved<T>()`), **filled** (a value
pointer), or **drained** (null).

`put` appends: it claims a slot with `auto slot = atom_end++` (an atomic
fetch-add, so concurrent appenders get distinct slots), materializes storage at
that slot via `Stash_T::get(..., true)`, and installs the value with the
allocate-then-CAS pattern. A chunk is filled with sentinels before it is
published, so a claimed slot reads as reserved until its value lands. The `key`
argument is unused at the leaf; ordering comes from the slot levels above it.

The walk parks `walk_cur` on the first reserved slot: it drains ready values
beyond it but never steps past an unfilled hole, so the leaf stays pinned until
the hole fills.

## `StashContext` and the key window

`StashContext` is the per-traversal state object passed by
reference through every operation. It holds:

- `Operation op` — `walk`, `peep`, or `clean`.
- `begin_key`, `end_key` — the half-open key window `[begin_key, end_key)` the
  current traversal covers. The walk loop advances `begin_key` as it drains
  ranges; `end_key == 0` means the window imposes no upper bound.
- `std::atomic_ullong atom_first_valid_key`, `atom_last_valid_key` — atomic
  bounds on where live data sits. `add` maintains them on the
  producer side; the walk advances `atom_first_valid_key` on the consumer side.

`check(key, limit_key)` is the loop predicate shared by the
traversal: it returns false when `end_key` is set and `key >= end_key`, when
`limit_key` is set and `key >= limit_key`, or when `key > atom_last_valid_key`.
Those three conditions are how a walk stops cleanly at the window edge, at a
parent-imposed sub-range edge, and past the last live key respectively.

The move constructor reloads the atomics into fresh atomics, and
the public constructor seeds all four key fields from a single `begin_key`, starting in `walk` mode.

## Traversal: walk, peep, clean

A traversal is a recursive descent. `StashSlots::next` drives the key levels;
`StashValues::next` drives the leaf. The op in `ctx` selects the behavior at
the leaf and at the clear-back step in the slots.

### `StashSlots::next`

`StashSlots::next(ctx, value_ptr, limit_key)` loops over the
key window. Each iteration:

1. Computes `new_first_valid_key = get_inc_base_key(ctx.begin_key)` (the start
   of the next slot) and `cur = get_slot(ctx.begin_key)`.
2. Looks up the child level with `Stash_T::get(&ptr_atom_ptr, cur, false)`
   (note `spawn == false`: the walk never allocates). `ChunkEmpty` means this
   slot is empty, so the loop continues; `StashShort`/`StashEmpty` means there
   is nothing materialized past here, so it jumps to the tail via
   `goto ret_next`.
3. If a child exists, it recurses: `ptr->next(ctx, value_ptr, new_first_valid_key)`,
   passing the next slot's base key as the child's `limit_key` so the child
   stays within this slot's key range.
4. If the child reports a hit (`status` true):
   - For `clean`, it swaps the slot pointer out with
     `atom_ptr.exchange(nullptr)` and deletes the now-drained child level
    . This is how an emptied subtree is reclaimed.
   - For `walk`/`peep`, it sets `found = true` and exits to `ret_next`
    .
5. Otherwise it advances: `loop = ctx.check(new_first_valid_key, limit_key)` and,
   if still in range, `ctx.begin_key = new_first_valid_key`.

At `ret_next`, for non-`peep` ops it bumps `ctx.begin_key`
forward to the base of the window/limit edge when nothing was found, then
advances `atom_first_valid_key` toward `get_dec_base_key(ctx.begin_key)` with a
CAS loop. That published bound is what lets the next walk pass
skip everything already drained. `peep` deliberately leaves the bounds untouched
so a look-ahead does not perturb the walk position.

### `StashValues::next`

`StashValues::next(ctx, value_ptr, _)` reads the leaf. It picks
the cursor by op: `clean_cur` for clean, `walk_cur` otherwise,
and the end of its range is `walk_cur` for clean (clean trails the walk) or
`atom_end` for walk/peep. Each iteration:

1. Looks up the slot with `spawn == false`; a missing chunk
   skips, a short/empty result returns false.
2. Advances the appropriate cursor by op: `peep` advances only its local `cur`,
   `walk` advances `walk_cur`, `clean` advances `clean_cur`.
3. Inspects the value pointer. For `walk`/`peep`, it tests `*ptr && **ptr`
   — the value must be a live, non-zero, dereferenceable
   pointer-like object. On a hit it copies into `*value_ptr` and marks
   `returning`.
4. For non-`peep` ops it clears the slot with `atom_ptr.exchange(nullptr)` and
   deletes the old value. So `walk` returns the value and
   consumes it; `clean` consumes without returning.
5. Returns true if it found a value to return.

### How slots are reclaimed

At the leaf, `walk` nulls each value it returns with `exchange(nullptr)` and
deletes it. Structure goes later, through `clean`: it drops a slot's whole
subtree once that slot lies entirely below `first_valid`, the walk's low-water
mark. `clean` is required; without it the structure grows without bound, since a
reused leaf's append cursor never resets.

`clean` is walk-based: its cutoff is `first_valid`, not a wall-clock time. Two
mechanisms make `first_valid` a true reclamation boundary:

- A leaf with an in-flight insert (a reserved slot not yet filled) reports itself
  pending, and the walk caps its `first_valid` advance at that leaf's key
  (`StashContext::pending_floor`). The low-water mark never crosses a leaf a
  producer is still filling.
- A late insert (a key that has just gone past) lowers `first_valid` to its own
  key before the value is published, so `clean` cannot reclaim under it.

Everything strictly below `first_valid` has been walked and is touched by no
producer, so its subtree drops with no locks and no time margin. This replaced a
margin-based `clean` (cutoff `now - margin`), which under load reclaimed
completed but overdue tasks, and the producer commit loop the sentinel frontier
removed; [STRAND_FIX.md](STRAND_FIX.md) records that change.

## Concurrency model

The structure is many-producer, single-consumer:

- **Producers (`add`/`put`)** are lock-free. Every structural mutation (a node,
  a chunk, a child level, a leaf slot, a value) goes through an atomic fetch-add
  (`atom_end++`) or an allocate-then-`compare_exchange` publish that discards the
  loser's allocation. No producer ever blocks another.
- **One consumer** runs `walk`, `peep`, and `clean`, one at a time. They mutate
  the non-atomic cursors and `ctx.begin_key`/`end_key`; two consumers, or `clean`
  concurrent with `walk`, is undefined behavior.
- **Walk vs. producers.** The walk reads with `spawn == false`, so it never races
  a producer into allocating the same chunk. A value appended after the walk
  passed its slot is picked up on the next pass, gated by `atom_last_valid_key`,
  which producers raise and the walk reads through `check`.

Correctness rests on four conditions:

1. **One consumer**, running `walk` then `clean` sequentially.
2. **The sentinel frontier.** A reserved slot holds the sentinel until its value
   lands, so the walk never skips a reserved slot, and `pending_floor` keeps
   reclamation from crossing a pending leaf.
3. **Insert latency below task lead time.** The bounds and the `first_valid`
   lowering happen during `add`, so a task that becomes due in the brief window
   before its insert publishes can be lost. Real schedules (leads of milliseconds
   against insert latencies of microseconds) never hit it; it appears only at a
   sub-millisecond leading edge or when a preempted producer's latency exceeds
   the lead. It is bounded loss, never a use-after-free.
4. **A span the consumer keeps up with.** The wheel reuses each physical slot
   every `Div * Mod` keys, one revolution. The consumer must finish a `clean`
   pass within one revolution; a producer reusing a slot a full revolution behind
   would alias a live subtree while `clean` frees it. Size the span well above one
   clean pass, as with any hierarchical timer wheel.

Within those conditions every entry fires exactly once, memory stays bounded, and
there are no races or use-after-free (ASan and TSan clean), including under
thread oversubscription. `test/longevity.cc` shows the reclamation (unbounded
growth without `clean`, bounded with it), and `test/concurrent.cc` checks the
accounting under heavy contention.

### Near-horizon scheduling: `horizon_margin`

`StashContext::horizon_margin` (0 by default, off) reserves a keep-out zone at
the wrap horizon: `add` rejects keys within that distance of the span, so a
near-horizon insert can never alias, through a level's modulus, onto a slot one
period below. Leave it at 0 unless you schedule within one period of the full
span (for example about 24 hours out on a 24-hour wheel); the cost is a
schedulable range of `span - horizon_margin`.

## Tracing as an injectable extension point

`stash.h` is instrumented throughout — every insert, walk iteration, hit, clear,
and break emits a trace line — but it carries none of the machinery to render
one. All of it flows through four hooks, and all four are no-ops by default:

- `L_STASH(fmt, args...)` — the main trace macro, fired on the insert and walk
  paths.
- `L_DEBUG_HOOK(label, fmt, args...)` — the per-iteration loop trace, with a
  label as its first argument.
- `L_EXC(msg)` — used once, to swallow exceptions in the manual destructor
 .
- `STASH_OP_COLOR(op)` — a color hook that maps a `StashContext::Operation` to a
  C string (an ANSI escape). `StashContext::_col()` returns it
  and the trace strings prepend it to tint a line per operation.

The defaults live in `stash_trace.h`, and `stash.h` reaches them through a
conditional include:

```cpp
#ifdef STASH_TRACE_HEADER
#  include STASH_TRACE_HEADER     // a consumer's header, e.g. Xapiand's
#else
#  include "stash_trace.h"        // the bundled no-op defaults
#endif
```

So a consumer points `STASH_TRACE_HEADER` at its own header (or defines the
macros before including `stash.h`) and gets real, colored, formatted tracing
back; otherwise the no-ops compile the instrumentation away to nothing. Every
hook in `stash_trace.h` is `#ifndef`-guarded, so overriding a subset is fine —
the rest fall back to the defaults. This is a small, deliberate extension point,
not a general logging framework: four named hooks, all optional.

The trace strings themselves build color in by concatenation, for example
`"StashSlots::" + CYAN + "LOOP" + CLEAR_COLOR`. The color tokens
(`CYAN`, `CLEAR_COLOR`, `FOREST_GREEN`, ...) appear only inside the arguments of
the trace macros. When those macros are the default no-ops, the arguments are
never evaluated, so the tokens cost nothing and need not exist at all — there is
no `CYAN` symbol anywhere in the default build. They only have to be defined when
a consumer enables tracing, and the consumer's trace header is exactly where they
get defined (see `examples/colored_trace/trace.h`). That is what lets `stash.h`
keep its colored Xapiand-style trace lines verbatim while depending on no color
header at all.

## Complexity

For a wheel of `L` nested slot levels:

- **Insert** (`add`/`put`) is `O(L)` slot computations plus, in the worst case,
  one chunk/node allocation per untouched level. For the leaf, `O(1)` amortized
  append (one fetch-add). When a single chunk overflows `Size`, `Data::get`
  walks `slot / Size` nodes, so a heavily populated single level degrades toward
  `O(slot / Size)` node hops.
- **Walk** visits each non-empty slot at most once per level; with the atomic
  key bounds it skips empty key ranges entirely, so a sparse wheel walks in time
  proportional to the live entries, not the key span.
- **Space** is proportional to the touched slots: chunks and nodes are allocated
  lazily, so an untouched key range costs nothing. A flat 24-hour wheel at 1 ms
  would preallocate 86 million buckets.

Under `P` concurrent producers, against the alternatives:

| | Insert under `P` producers | Memory | Answers "due now?" |
| --- | --- | --- | --- |
| **stash** | `O(1)` amortized, lock-free | `O(N)`, sparse | yes (slot-quantized) |
| heap + mutex | `O(P log N)`, serialized on one lock | `O(N)` | yes (exact order) |
| flat timer wheel | `O(1)` | `O(range / resolution)`, preallocated | yes (slot-quantized) |
| lock-free queue | `O(1)` | `O(N)` | no |

Keys converging on one instant degrade the leaf append toward `O(b)` for `b`
values in that bucket. [bench/RESULTS.md](bench/RESULTS.md) measures the tail
latencies.

## Design decisions and trade-offs

- **Pointer-indirect slots.** Every slot is an `std::atomic<T*>` rather than an
  inline value. That is what makes lock-free publish (allocate-then-CAS) and
  lock-free clear (`exchange(nullptr)`) possible, at the cost of a heap
  allocation and an indirection per live entry.
- **Chunked over a flat array.** A single flat `std::array` would force a fixed
  capacity up front. The `Size`-chunk-plus-linked-node layout lets a level grow
  past `Size` without reallocating or copying, and untouched ranges stay
  unallocated.
- **Caller-supplied keys, not wall clock.** The key is an abstract unsigned
  integer supplied by the caller, so the same structure works for a real
  millisecond clock, a logical tick, or a test's deterministic counter.
- **Three ops over one traversal.** Folding walk/peep/clean into a single
  `next` with an op flag keeps the recursion in one place; peep is a
  non-destructive walk, clean is a destructive sweep that returns nothing.

## Known limitations and sharp edges

- **Value type must be pointer-like.** The walk tests `*ptr && **ptr`
 , so values must be bool-testable and dereferenceable
  (`std::shared_ptr<T>` is the canonical choice). A "zero/empty" value reads as
  absent, so storing one as a live entry silently loses it.
- **Single-consumer only.** The walk/clean cursors are non-atomic; concurrent
  walkers corrupt them. There is no internal guard against this.
- **Pending leaves pin the walk.** A producer preempted between reserving and
  filling a slot holds its leaf, and everything after it in that leaf, until it
  resumes; the values are re-drained on the next pass, never lost.
- **Manual destructor teardown.** `Data::~Data` tears the
  structure down by hand: it walks and deletes the `atom_next` chain, then
  deletes every value pointer in the chunk, then the chunk. It swallows
  exceptions through `L_EXC`. Destruction is not concurrency-safe and assumes no walker
  or producer is active.
- **Overflow is a hard error.** `add` throws `std::out_of_range` when a key is
  more than one wheel span (less `horizon_margin`) past `atom_first_valid_key`.
  The caller (a scheduler) keeps keys within the live window by draining the
  wheel as the clock advances.

## Possible improvements

- Replace the hand-rolled `Data::~Data` teardown with RAII-owning node/chunk
  types so destruction is harder to get wrong.
- Make the walk cursors atomic (or document a clear ownership handoff) if more
  than one consumer is ever needed.
- Pool or recycle the per-entry allocations to cut the allocate/free churn on a
  hot scheduler path.

## Standalone vs. Xapiand

This repository is a standalone extraction of `stash.h` from
[Xapiand](https://github.com/Kronuz/Xapiand). The dependency on Xapiand's
`log.h` and color palette was cut by routing all tracing through the injectable
hooks described above (see "Tracing as an injectable extension point"):
`#include "log.h"` became the conditional include of a trace header, defaulting to the no-op `stash_trace.h`, and `_col()` resolves
through `STASH_OP_COLOR`, which is `""` by default. The colored,
formatted tracing Xapiand uses is fully recoverable by injecting a trace header,
as `examples/colored_trace/` shows.

The data-structure logic is otherwise unchanged. For the full timer-wheel /
scheduler design, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).
