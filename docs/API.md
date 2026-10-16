# stash API reference

## `StashContext`

Carries the state for a single traversal.

- `Operation op` — `walk`, `peep`, or `clean`. Walk reads and consumes; peep
  reads without consuming; clean only reclaims cleared slots.
- `begin_key` / `end_key` — the half-open key window `[begin_key, end_key)` the
  traversal covers. `end_key == 0` means "no upper bound from the window."
- `atom_first_valid_key` / `atom_last_valid_key` — atomic bounds on where live
  data is. `add` widens them; the walk advances `atom_first_valid_key` so the
  next pass can skip ranges already drained.
- `horizon_margin` — keys within this distance of the wheel's span are
  rejected by `add`, so a near-horizon insert cannot alias a slot one period
  below. 0 (off) by default; see ARCHITECTURE.md.
- `pending_floor` — the lowest key of a leaf seen pending during a walk; the walk
  caps its `first_valid` advance there. The consumer resets it to
  `StashContext::IDLE` before each walk.
- `check(key, limit_key)` — the loop predicate: stops when `key` reaches
  `end_key`, `limit_key`, or passes `atom_last_valid_key`.

## `Stash<T, Size>`

The chunked atomic-pointer array primitive. The leaf's base,
`Stash<T, Size, true>`, fills new chunks with the reserved sentinel.

- `get(pptr_atom_ptr, slot, spawn)` — resolves `slot` to the address of an
  `std::atomic<T*>`. With `spawn == true` it lazily allocates chunks and linked
  nodes via CAS; with `spawn == false` it reports `StashEmpty`, `ChunkEmpty`,
  or `StashShort` instead of allocating.

## `StashSlots<T, Size, Div, Mod>`

A keyed level over a `Stash` whose element type `T` is itself a `Stash`
(another `StashSlots` or a `StashValues`).

- `put(ctx, key, args...)` — maps `key` to slot `(key / Div) % Mod`, lazily
  creates the child level, and recurses.
- `add(ctx, key, args...)` — `put` plus bound maintenance; throws
  `std::out_of_range("stash overflow")` if `key` is beyond the wheel span
  `[first_valid_key, first_valid_key + Div*Mod)`.
- `next(ctx, value_ptr [, limit_key])` — walks the key window in `ctx.op` mode,
  recursing into the child level, returning the first matching value.

## `StashValues<T, Size>`

The append-only leaf level.

- `put(ctx, key, args...)` — appends, bumping the atomic write cursor
  `atom_end`. The leaf ignores `key`; ordering comes from the
  slot levels above it.
- `next(ctx, value_ptr, limit_key)` — reads from `walk_cur` (or `clean_cur` for
  clean), returns the first non-empty pointer-like value, and consumes it
  unless the op is `peep`.

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
  trace line per `StashContext::Operation`. `StashContext::_col()` returns this. It is `""` by default.

The bundled `stash_trace.h` supplies the no-op defaults, each one
`#ifndef`-guarded so you can override any subset and let the rest fall back.
There are two ways to plug in real tracing:

1. Point `STASH_TRACE_HEADER` at a header that defines the hooks. `stash.h`
   includes it in place of `stash_trace.h`:

   ```sh
   c++ -std=c++20 -DSTASH_TRACE_HEADER='"my_trace.h"' ...
   ```

2. Define the macros directly before including `stash.h`.

Nothing is required by default. A complete, runnable override lives in
[`examples/colored_trace/`](../examples/colored_trace/): it defines the color
constants, `STASH_OP_COLOR`, and the `L_*` macros, rendering with `std::format`
to produce colored, fully-formatted trace lines. That example needs C++20 for
`std::format` / `std::vformat`, even though `stash.h` itself only requires C++17.
It is how a consumer such as Xapiand recovers the colored debug tracing it relies
on without editing `stash.h`.
