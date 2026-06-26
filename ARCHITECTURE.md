# Architecture

This document describes the internal design of `stash`: how the three templates
compose, the concurrency model, the traversal machinery, and the trade-offs
baked into the data structure. All file references point at `stash.h` unless
noted otherwise.

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

`Stash<T, Size>` (`stash.h:113`) owns a single `Data data` member
(`stash.h:201`) and forwards `get` to it (`stash.h:209`). All the real work is
in the nested `Data` class (`stash.h:116`).

### The chunked atomic-pointer array

A `Data` node holds:

- `std::atomic<Chunks*> atom_chunk`, where
  `Chunks = std::array<std::atomic<_Tp*>, _Size>` (`stash.h:117`). This is one
  chunk: a fixed-size array of `Size` atomic element pointers.
- `std::atomic<Data*> atom_next` — a pointer to the next node in a linked list
  of nodes (`stash.h:119`).

So the logical array is a linked list of `Data` nodes, each holding (at most)
one chunk of `Size` slots. Logical slot `N` lives in node `N / Size` at offset
`N % Size`. Both the chunk and the next node start as `nullptr` and are
allocated only when a slot in that range is first touched.

### CAS-based lazy growth in `Data::get`

`Data::get(pptr_atom_ptr, slot, spawn)` (`stash.h:154`) resolves a logical slot
to the address of its `std::atomic<_Tp*>`, allocating storage along the way when
`spawn` is true. It walks the structure in two phases.

First, the early-out: if `spawn` is false and the node has neither a next node
nor a chunk, it returns `StashState::StashEmpty` (`stash.h:155`). This is how a
read-side walk cheaply discovers an untouched subtree.

Second, node traversal. If `slot >= _Size`, the target is in a later node, so it
computes `chunk_num = slot / _Size` and reduces `slot` to `slot % _Size`
(`stash.h:160`), then walks `chunk_num` hops down the `atom_next` chain
(`stash.h:164`). At each hop:

- If the next node does not exist and `spawn` is false, it returns
  `StashState::StashShort` (`stash.h:169`): the read side has run off the end of
  the materialized list.
- If the next node does not exist and `spawn` is true, it allocates a fresh
  `Data` and publishes it with
  `atom_next.compare_exchange_strong(next, tmp)` (`stash.h:171`). If the CAS
  loses to a racing producer, it deletes its own `tmp` and adopts the winner's
  node. Either way it advances to a valid `next`.

Third, chunk allocation. Once on the right node, it loads `atom_chunk`. If the
chunk is missing and `spawn` is false, it returns `StashState::ChunkEmpty`
(`stash.h:183`). If `spawn` is true, it allocates a zero-initialized
`Chunks{{ }}` and publishes it with `atom_chunk.compare_exchange_strong`
(`stash.h:187`), again deleting its own allocation if it loses the race.

Finally it returns the address of `(*chunk)[slot]` (`stash.h:194`) and
`StashState::Ok`. The caller then operates on that single `std::atomic<_Tp*>`.

The pattern repeats at every level of the structure: never lock, always
allocate-then-CAS-then-reconcile. A losing producer never corrupts state; it
just discards its speculative allocation and continues with whatever the winner
installed.

## The keyed level: `StashSlots`

`StashSlots<_Tp, _Size, _Div, _Mod>` (`stash.h:220`) derives
from `Stash<_Tp, _Size>`, so it has the chunked array underneath. Its job is to
turn a key into a slot index and recurse into the child level stored there.

### Key-to-slot mapping and nesting

The slot for a key is `(key / _Div) % _Mod` (`get_slot`, `stash.h:240`). `_Div`
is the resolution of this level (how many key units map to one slot), and `_Mod`
is the number of slots. A few helpers derive key boundaries from this:

- `get_base_key(key) = (key / _Div) * _Div` — the key at the start of the slot
  `key` falls in (`stash.h:224`).
- `get_inc_base_key(key) = get_base_key(key) + _Div` — the start of the next
  slot (`stash.h:228`).
- `get_dec_base_key(key) = get_base_key(key) - _Div` — the start of the previous
  slot (`stash.h:232`).
- `get_end_base_key(key) = get_base_key(key) + _Div * _Mod` — one wheel span
  past the slot's base, used as the overflow boundary (`stash.h:236`).

Because the element type `_Tp` of a `StashSlots` is itself a `Stash` (a finer
`StashSlots`, or the `StashValues` leaf), the levels nest. The outermost level
divides the key space coarsely; each nested level refines the residual. With the
Xapiand stack, the top level buckets into 18-second slots, and the chain down
narrows resolution to 1 ms at the leaf. The `_Div` values multiply down the
chain so each level's span equals the next level's slot width.

### Inserting: `put` and `add`

`put(ctx, key, args...)` (`stash.h:332`) computes the slot, calls
`Stash_T::get(&ptr_atom_ptr, slot, true)` to materialize storage, then ensures a
child level exists in that slot: if the slot's pointer is null it allocates a
`new _Tp()` and installs it with `compare_exchange_strong`, deleting its own
allocation on a lost race (`stash.h:342`). Then it forwards `put` to the child
(`stash.h:351`). The recursion bottoms out at `StashValues::put`.

`add(ctx, key, args...)` (`stash.h:354`) is `put` with bounds checking and
bound maintenance. Before inserting it checks
`key >= get_end_base_key(ctx.atom_first_valid_key.load())` and throws
`std::out_of_range("stash overflow")` if the key is more than one wheel span
past the first valid key (`stash.h:356`). After inserting, it widens the context
bounds with CAS loops: it lowers `atom_first_valid_key` toward `key` and raises
`atom_last_valid_key` toward `key` (`stash.h:362`). These bounds are what later
let the walk skip empty key ranges.

## The leaf: `StashValues`

`StashValues<_Tp, _Size>` (`stash.h:371`) also derives from
`Stash<_Tp, _Size>`, but it ignores the key entirely. It is an append-only log.

It adds three cursors (`stash.h:375`):

- `std::atomic_size_t atom_end` — the write cursor / count of appended slots.
- `size_t walk_cur` — the walk/peep read cursor (non-atomic).
- `size_t clean_cur` — the clean read cursor (non-atomic).

`put` (`stash.h:462`) appends: it claims a slot with `auto slot = atom_end++`
(an atomic fetch-add, so concurrent appenders get distinct slots), materializes
storage at that slot via `Stash_T::get(..., true)`, and installs the new value
with the allocate-then-CAS pattern (`stash.h:472`). The `key` argument is
unused at the leaf; ordering is entirely determined by the slot levels above it.

The two read cursors keep walk and clean independent. The walk advances
`walk_cur` as it consumes; the clean pass can only reclaim up to where the walk
has already been. This is the mechanism that makes clean safe to run behind the
walk without racing it on the same entries.

## `StashContext` and the key window

`StashContext` (`stash.h:47`) is the per-traversal state object passed by
reference through every operation. It holds:

- `Operation op` — `walk`, `peep`, or `clean` (`stash.h:48`).
- `begin_key`, `end_key` — the half-open key window `[begin_key, end_key)` the
  current traversal covers. The walk loop advances `begin_key` as it drains
  ranges; `end_key == 0` means the window imposes no upper bound.
- `std::atomic_ullong atom_first_valid_key`, `atom_last_valid_key` — atomic
  bounds on where live data sits (`stash.h:59`). `add` maintains them on the
  producer side; the walk advances `atom_first_valid_key` on the consumer side.

`check(key, limit_key)` (`stash.h:76`) is the loop predicate shared by the
traversal: it returns false when `end_key` is set and `key >= end_key`, when
`limit_key` is set and `key >= limit_key`, or when `key > atom_last_valid_key`.
Those three conditions are how a walk stops cleanly at the window edge, at a
parent-imposed sub-range edge, and past the last live key respectively.

The move constructor reloads the atomics into fresh atomics (`stash.h:62`), and
the public constructor seeds all four key fields from a single `begin_key`
(`stash.h:69`), starting in `walk` mode.

## Traversal: walk, peep, clean

A traversal is a recursive descent. `StashSlots::next` drives the key levels;
`StashValues::next` drives the leaf. The op in `ctx` selects the behavior at
the leaf and at the clear-back step in the slots.

### `StashSlots::next`

`StashSlots::next(ctx, value_ptr, limit_key)` (`stash.h:250`) loops over the
key window. Each iteration (`stash.h:256`):

1. Computes `new_first_valid_key = get_inc_base_key(ctx.begin_key)` (the start
   of the next slot) and `cur = get_slot(ctx.begin_key)`.
2. Looks up the child level with `Stash_T::get(&ptr_atom_ptr, cur, false)`
   (note `spawn == false`: the walk never allocates). `ChunkEmpty` means this
   slot is empty, so the loop continues; `StashShort`/`StashEmpty` means there
   is nothing materialized past here, so it jumps to the tail via
   `goto ret_next` (`stash.h:269`).
3. If a child exists, it recurses: `ptr->next(ctx, value_ptr, new_first_valid_key)`,
   passing the next slot's base key as the child's `limit_key` so the child
   stays within this slot's key range (`stash.h:279`).
4. If the child reports a hit (`status` true):
   - For `clean`, it swaps the slot pointer out with
     `atom_ptr.exchange(nullptr)` and deletes the now-drained child level
     (`stash.h:282`). This is how an emptied subtree is reclaimed.
   - For `walk`/`peep`, it sets `found = true` and exits to `ret_next`
     (`stash.h:288`).
5. Otherwise it advances: `loop = ctx.check(new_first_valid_key, limit_key)` and,
   if still in range, `ctx.begin_key = new_first_valid_key` (`stash.h:296`).

At `ret_next` (`stash.h:305`), for non-`peep` ops it bumps `ctx.begin_key`
forward to the base of the window/limit edge when nothing was found, then
advances `atom_first_valid_key` toward `get_dec_base_key(ctx.begin_key)` with a
CAS loop (`stash.h:319`). That published bound is what lets the next walk pass
skip everything already drained. `peep` deliberately leaves the bounds untouched
so a look-ahead does not perturb the walk position.

### `StashValues::next`

`StashValues::next(ctx, value_ptr, _)` (`stash.h:391`) reads the leaf. It picks
the cursor by op: `clean_cur` for clean, `walk_cur` otherwise (`stash.h:393`),
and the end of its range is `walk_cur` for clean (clean trails the walk) or
`atom_end` for walk/peep (`stash.h:395`). Each iteration:

1. Looks up the slot with `spawn == false` (`stash.h:403`); a missing chunk
   skips, a short/empty result returns false.
2. Advances the appropriate cursor by op: `peep` advances only its local `cur`,
   `walk` advances `walk_cur`, `clean` advances `clean_cur` (`stash.h:417`).
3. Inspects the value pointer. For `walk`/`peep`, it tests `*ptr && **ptr`
   (`stash.h:437`) — the value must be a live, non-zero, dereferenceable
   pointer-like object. On a hit it copies into `*value_ptr` and marks
   `returning` (`stash.h:439`).
4. For non-`peep` ops it clears the slot with `atom_ptr.exchange(nullptr)` and
   deletes the old value (`stash.h:445`). So `walk` returns the value and
   consumes it; `clean` consumes without returning.
5. Returns true if it found a value to return (`stash.h:452`).

### How cleared slots are reclaimed

Reclamation happens at two granularities. At the leaf, `walk` and `clean` both
null out the entry with `exchange(nullptr)` and `delete` the old value
(`stash.h:446`). At the slot levels, once a child level reports that it is fully
drained, `clean` swaps the child pointer out and deletes the whole child
(`stash.h:282`), collapsing the emptied subtree. The split between `walk_cur`
and `clean_cur` at the leaf is what makes the deferred clean pass safe: clean
only ever touches entries the walk has already moved past.

## Concurrency model

The structure is many-producer, single-consumer:

- **Producers (`add`/`put`)** are lock-free. Every structural mutation — adding
  a node, allocating a chunk, installing a child level, claiming a leaf slot,
  installing a value — goes through either an atomic fetch-add (`atom_end++`,
  `stash.h:464`) or an allocate-then-`compare_exchange` publish that discards
  the loser's allocation. No producer ever blocks another.

- **The consumer (`walk`/`peep`/`clean`)** is single-threaded by contract. It
  reads slots with atomic loads, but it mutates the non-atomic cursors
  `walk_cur` and `clean_cur` (`stash.h:375`) and advances the non-atomic
  `ctx.begin_key`/`end_key`. Two concurrent walkers on the same structure would
  race those cursors. It is safe to run exactly one walker.

- **Walk vs. producers.** The walk reads with `spawn == false`, so it never
  races a producer into allocating the same chunk. A value appended after the
  walk has passed its slot is simply picked up on the next pass, gated by
  `atom_last_valid_key`, which producers raise and the walk reads through
  `check` (`stash.h:85`).

## Tracing as an injectable extension point

`stash.h` is instrumented throughout — every insert, walk iteration, hit, clear,
and break emits a trace line — but it carries none of the machinery to render
one. All of it flows through four hooks, and all four are no-ops by default:

- `L_STASH(fmt, args...)` — the main trace macro, fired on the insert and walk
  paths.
- `L_DEBUG_HOOK(label, fmt, args...)` — the per-iteration loop trace, with a
  label as its first argument (`stash.h:270`, `stash.h:410`).
- `L_EXC(msg)` — used once, to swallow exceptions in the manual destructor
  (`stash.h:160`).
- `STASH_OP_COLOR(op)` — a color hook that maps a `StashContext::Operation` to a
  C string (an ANSI escape). `StashContext::_col()` returns it (`stash.h:117`)
  and the trace strings prepend it to tint a line per operation.

The defaults live in `stash_trace.h`, and `stash.h` reaches them through a
conditional include (`stash.h:34`):

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
`"StashSlots::" + CYAN + "LOOP" + CLEAR_COLOR` (`stash.h:270`). The color tokens
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
  lazily, so an untouched key range costs nothing.

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
  (`stash.h:437`), so values must be bool-testable and dereferenceable
  (`std::shared_ptr<T>` is the canonical choice). A "zero/empty" value reads as
  absent, so storing one as a live entry silently loses it.
- **Single-consumer only.** The walk/clean cursors are non-atomic; concurrent
  walkers corrupt them. There is no internal guard against this.
- **Manual destructor teardown.** `Data::~Data` (`stash.h:130`) tears the
  structure down by hand: it walks and deletes the `atom_next` chain, then
  deletes every value pointer in the chunk, then the chunk. It swallows
  exceptions through `L_EXC` (`stash.h:149`, a no-op by default in the
  standalone build). Destruction is not concurrency-safe and assumes no walker
  or producer is active.
- **Overflow is a hard error.** `add` throws `std::out_of_range` when a key is
  more than one wheel span past `atom_first_valid_key` (`stash.h:356`). The
  caller (a scheduler) is expected to keep keys within the live window by
  draining the wheel as the clock advances.

## Possible improvements

- Add a ring-buffer mode so the wheel can wrap the key space instead of throwing
  on overflow.
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
`#include "log.h"` became the conditional include of a trace header
(`stash.h:34`), defaulting to the no-op `stash_trace.h`, and `_col()` resolves
through `STASH_OP_COLOR`, which is `""` by default (`stash.h:117`). The colored,
formatted tracing Xapiand uses is fully recoverable by injecting a trace header,
as `examples/colored_trace/` shows.

The data-structure logic is otherwise unchanged. For the full timer-wheel /
scheduler design, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).
