# AGENTS.md

Working notes for anyone changing this repository. [README.md](README.md) is for
users; [ARCHITECTURE.md](ARCHITECTURE.md) explains the design. This file covers
the layout, how to build and test, the invariants a change must keep, and the
traps.

## Repo map

```
stash.h                  The library: Stash, StashSlots, StashValues, StashContext. Header-only.
stash_trace.h            No-op default tracing and color hooks.
test/test.cc             Smoke and regression tests (CTest: stash).
test/concurrent.cc       Many producers and one walking, cleaning consumer: every task fires once.
test/longevity.cc        Wheel memory over many wraparounds: bounded with clean, growing without.
examples/demo.cc         A tour of the structure (target stash_demo).
examples/colored_trace/  Injecting a trace header for colored tracing.
bench/                   Benchmarks against a locked heap, Asio timers, and a lock-free queue.
docs/API.md              The API reference.
docs/stash.d2            The README diagram's source; docs/stash.svg is rendered from it.
STRAND_FIX.md            The record of the sentinel-frontier change.
```

## Build and test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The `stash` target (alias `stash::stash`) is an `INTERFACE` library requesting
`cxx_std_17`. CTest runs `stash_test`, which prints
`stash OK: walked 3 values in key order: 11 22 33`.

`test/concurrent.cc` runs in CTest as `stash_concurrent` (4 producers, 2 s, tasks due at
`now()`), so the CI sanitizer jobs cover reclamation and the descent window. Run it
longer by hand with `build/stash_concurrent PRODUCERS SECONDS`, or repeat it
oversubscribed with `test/stress.sh build/stash_concurrent RUNS PRODUCERS SECONDS
LEAD_MS HOGS` (HOGS nice'd busy loops preempt producers mid-insert, which macOS cannot
force with `taskset`). `test/longevity.cc` is a manual tool (it replaces global `new`,
so not under ASan, and always exits 0):

```sh
c++ -std=c++17 -O2 -pthread -I. test/longevity.cc -o longevity && ./longevity
```

To redraw the README diagram:
`d2 --layout elk --sketch --theme 302 docs/stash.d2 docs/stash.svg`.

## Conventions

- **C++17, header-only.** Nothing that needs a `.cc` or a link step.
- **Lock-free atomics.** Shared mutation goes through `std::atomic` with
  `compare_exchange`, `exchange`, or fetch-add. No mutexes or blocking.
- **No dependencies.** `stash.h` includes only standard headers and the trace
  header.
- **Tracing goes through four hooks**, no-ops by default: `L_STASH`,
  `L_DEBUG_HOOK` (label first), `L_EXC`, and `STASH_OP_COLOR(op)`. Each default
  in `stash_trace.h` is `#ifndef`-guarded. A consumer injects its own with
  `-DSTASH_TRACE_HEADER='"my_trace.h"'` or defines the macros first. Color tokens
  such as `CYAN` appear only inside trace-macro arguments, so they need not exist
  in the default build; never reference them outside a trace call.
- Double quotes in code; no em dashes in prose.

## Load-bearing invariants

Breaking one of these produces silent loss or a race, not a compile error.

- **Slot and key math.** A `StashSlots` key maps to slot `(key / _Div) % _Mod`
  (`get_slot`), and `get_base_key`, `get_inc_base_key`, `get_dec_base_key`, and
  `get_end_base_key` derive from `_Div` and `_Mod`. The walk uses them to set
  child `limit_key`s and to advance `atom_first_valid_key`.
- **Publish via CAS.** Every structural install (a `Data` node, a chunk, a child
  level, a leaf value) allocates, `compare_exchange_strong`s, and on a lost race
  deletes its own allocation and adopts the winner's.
- **One consumer.** `walk`, `peep`, and `clean` run on one thread, one at a time:
  `walk_cur`, `clean_cur`, and `ctx.begin_key`/`end_key` are plain fields.
  Producers may run concurrently with each other and with that consumer.
- **The walk never allocates.** Every read-side `Stash_T::get` passes
  `spawn == false` and branches on `StashState`.
- **The sentinel frontier.** A leaf chunk is filled with `stash_reserved<T>()`
  before it is published, and a reserved slot keeps the sentinel until its value
  lands. The walk parks on the first sentinel and never steps past it.
- **`first_valid` is the reclamation boundary, and only the walk writes it.** The
  walk caps its advance at `pending_floor` (the lowest pending leaf seen this walk)
  and at `first_valid_cap` (the lowest in-flight announce it read); `clean` frees
  only subtrees entirely below it. The consumer resets `pending_floor` to `IDLE`
  at the start of each walk.
- **Announce before descending.** `add` claims an announce slot (seq_cst) before
  its first pointer load, then reads `atom_insert_floor` (seq_cst) and files a key
  below it at the floor. The outermost `StashSlots::next` of a call that starts a
  walk at `first_valid` stores the floor (seq_cst) before it reads the slots or
  the tree; a later call's snapshot would not cover ranges the walk already
  passed. That store-load pair keeps `first_valid` at or below every insert in
  flight; weakening either side to acquire/release reopens the descent window.
  `first_valid <= atom_insert_floor` always.
- **Pointer-like, non-null values.** The walk's liveness test is `*ptr && **ptr`;
  a null or "zero" value reads as absent.

## How to extend

- **Add a resolution.** Nest another `StashSlots` with its own `_Div` and `_Mod`,
  each level's span (`_Div * _Mod`) equal to the next level's slot width. Xapiand
  uses `50x1ms -> 10x50ms -> 36x500ms -> 4800x18s`.
- **Change the value type.** Any pointer-like, bool-testable, dereferenceable
  type works; check what `*ptr && **ptr` means for it.
- **Grow the tests with the change.** A behavioral change adds an assertion to
  `test/test.cc`; a change to insert, walk, or `clean` also runs
  `test/concurrent.cc` under TSan and ASan.

## Traps

- **A "zero" value is silently lost.** It reads as absent.
- **Two consumers corrupt the cursors.** Nothing stops it at compile time.
- **A lost CAS must delete its allocation.** Omitting it leaks; reusing the
  loser's pointer is a use-after-free.
- **`clean` is required.** Without it the structure grows without bound.
- **Size the span above one clean pass.** A wheel the consumer laps aliases a live
  subtree while `clean` frees it.
- **Overflow throws.** `add` throws `std::out_of_range("stash overflow")` past one
  span (less `horizon_margin`) from `atom_first_valid_key`; the scheduler drains
  as its clock advances.
- **Insert through `add`, not `put`.** `put` skips the announce, so a `put`
  racing the walk can be skipped or freed under; `add` is the concurrent entry
  point.
- **A stalled producer pins `first_valid`.** Its announce holds the low-water mark
  (and so `clean` and the horizon) until it resumes; walks keep firing above it.
- **Destruction is manual and not concurrent.** `Data::~Data` frees the node
  chain and chunks by hand; destroy a structure only when no producer or consumer
  touches it.

## Standalone vs. Xapiand

This is `stash.h` extracted from [Xapiand](https://github.com/Kronuz/Xapiand),
with Xapiand's `log.h` and color palette replaced by the injectable trace hooks.
Since the extraction it gained the sentinel frontier and walk-based `clean`
([STRAND_FIX.md](STRAND_FIX.md)) and the in-flight announce (ARCHITECTURE.md,
"Inserts in flight"). Keep extraction hygiene separate from algorithm
changes so the two can be reconciled with Xapiand.
