# AGENTS.md

Working notes for agents modifying this repository. For the data-structure
design read `ARCHITECTURE.md`; for usage read `README.md`. This file covers the
repo layout, how to build and test, the invariants you must not break, and the
traps that are easy to fall into.

## Repo map

```
stash.h            The library. Three templates plus StashContext. Header-only.
stash_trace.h      No-op L_* trace macro stubs (stands in for Xapiand's log.h).
test/test.cc       Runnable smoke test: a single-level wheel over a leaf.
CMakeLists.txt     INTERFACE library target `stash` + CTest test `stash`.
LICENSE            MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md          What it is, install, usage, API reference, caveats.
ARCHITECTURE.md    Internal design, concurrency model, trade-offs.
```

Everything ships in `stash.h`. There is no `.cc` to compile except the test.

## Build and run the test

```sh
c++ -std=c++17 -I. test/test.cc -o test/test && ./test/test
```

Expected output: `stash OK: walked 3 values in key order: 11 22 33`, exit 0.

Or via CMake/CTest:

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The CMake `stash` target is an `INTERFACE` library (`CMakeLists.txt:4`) that
only adds the include directory and requests `cxx_std_17`. The test target is
`stash_test`; the registered CTest test name is `stash`.

## Conventions

- **C++17**, header-only. Keep it that way. Anything that would force a `.cc`
  translation unit or a link step does not belong here.
- **Lock-free atomics.** All shared mutation goes through `std::atomic` with
  `compare_exchange` / `exchange` / fetch-add. Do not introduce mutexes,
  condition variables, or any blocking primitive.
- **No external dependencies.** The only includes are `<array>`, `<atomic>`,
  `<cassert>`, and the local `stash_trace.h` (`stash.h:25`). Do not add new
  third-party or Xapiand headers.
- **Tracing is opt-in.** Code may call `L_STASH`, `L_DEBUG_HOOK`, `L_EXC`, etc.
  These resolve to no-ops via `stash_trace.h` unless the consumer defines them
  first. Keep new trace calls behind the same macros; never assume they do
  anything.
- Double quotes in code; no em dashes in prose.

## Load-bearing invariants

These are the rules the structure's correctness rests on. Breaking one tends to
produce silent data loss or a race, not a compile error.

- **Slot/key math.** A `StashSlots` key maps to slot `(key / _Div) % _Mod`
  (`stash.h:240`), and the per-level key boundaries (`get_base_key`,
  `get_inc_base_key`, `get_end_base_key`) are all derived from `_Div`/`_Mod`
  (`stash.h:224`). The walk relies on these to set child `limit_key`s and to
  advance `atom_first_valid_key`. Change the formula and the wheel mis-buckets
  or the walk skips live keys.
- **Publish via CAS.** Every structural install — a `Data` node (`stash.h:171`),
  a chunk (`stash.h:187`), a child level (`stash.h:344`), a leaf value
  (`stash.h:474`) — follows allocate, `compare_exchange_strong`, and on a lost
  race `delete` the loser's allocation and adopt the winner. Never store a raw
  pointer without the CAS, and never leak the loser.
- **Single-consumer walk.** `walk_cur` and `clean_cur` are non-atomic
  (`stash.h:375`); `ctx.begin_key`/`end_key` are plain fields. Exactly one
  thread may walk/peep/clean a given structure at a time. Producers
  (`add`/`put`) may run concurrently with each other and with the single walker.
- **Walk reads with `spawn == false`.** The walk must never allocate. Every
  read-side `Stash_T::get` passes `false` (`stash.h:263`, `stash.h:403`) and
  branches on `StashState`. Passing `true` on the walk path would have the
  consumer racing producers into allocation.
- **Pointer-like, non-zero values.** The walk's liveness test is `*ptr && **ptr`
  (`stash.h:437`). Values must be bool-testable and dereferenceable, and a
  zero/empty value reads as absent.
- **Key-window bounds.** `add` widens `atom_first_valid_key`/`atom_last_valid_key`
  (`stash.h:362`); the walk advances `atom_first_valid_key` (`stash.h:319`).
  `check` (`stash.h:76`) gates the loop on `end_key`, `limit_key`, and
  `atom_last_valid_key`. Keep these in sync or the walk either stops early or
  scans dead ranges.

## How to extend

- **Add a wheel resolution.** Nest another `StashSlots` in the type chain with
  its own `_Div`/`_Mod`, ensuring each level's span (`_Div * _Mod`) equals the
  next level's slot width. The leaf stays `StashValues`. See the Xapiand stack
  in `ARCHITECTURE.md` (`50x1ms -> 10x50ms -> 36x500ms -> 4800x18s`).
- **Change the value type.** Any pointer-like, bool-testable, dereferenceable
  type works (`std::shared_ptr<T>` is the reference choice). Verify the walk's
  `*ptr && **ptr` test means what you intend for the new type.
- **Plug in tracing.** Define `L_STASH`, `L_DEBUG_HOOK`, `L_EXC`, and the color
  symbols the trace strings reference before including `stash.h`, instead of
  letting `stash_trace.h` stub them out.
- **Always extend the smoke test.** `test/test.cc` is the only executable check
  in the repo. Any behavioral change should grow a corresponding assertion
  there.

## Traps

- **Don't store a "zero" value.** It reads as absent and is silently lost. This
  is the single easiest mistake.
- **Don't run two walkers.** Nothing stops you at compile time; the non-atomic
  cursors will corrupt.
- **Don't drop the loser's allocation handling.** On a lost CAS the speculative
  `new` must be `delete`d (`stash.h:174`, `stash.h:190`, `stash.h:347`,
  `stash.h:477`). Omitting it leaks; reusing the loser's pointer is a
  use-after-free waiting to happen.
- **Mind the overflow.** `add` throws `std::out_of_range("stash overflow")` when
  a key is more than one wheel span past `atom_first_valid_key` (`stash.h:356`).
  The driving scheduler is responsible for draining as the clock advances.
- **The destructor is manual and not concurrency-safe.** `Data::~Data`
  (`stash.h:130`) hand-walks and frees the node chain and chunk. Do not destroy
  a structure while a walker or producer is touching it.
- **`_CurrentKey` and `_Ring` are vestigial.** They are declared but unused in
  the bodies. Do not write code that assumes they do anything; if you wire them
  up, that is a real behavior change to call out.

## Standalone vs. Xapiand

This is a standalone extraction of `stash.h` from
[Xapiand](https://github.com/Kronuz/Xapiand). Two deltas from the original:
`#include "log.h"` became the local `stash_trace.h` (`stash.h:29`), and
`StashContext::_col()` now returns `""` to drop the ANSI-color dependency
(`stash.h:107`). The data-structure logic is otherwise identical. Keep changes
that are pure extraction hygiene clearly separated from changes to the algorithm
so they can be reconciled with upstream. For the scheduler design this powers,
see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).
