# stash

A lock-free, hierarchical **slot store** — the data structure underneath a
multi-resolution timer wheel. Producers insert values keyed by an unsigned
integer using atomic compare-and-swap (no locks); a single consumer walks a key
window in order, optionally consuming or garbage-collecting as it goes.

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where it's the
engine behind the scheduler thread (which drives the async logger and every
debounced background job — fsync, commit, replication triggers). It's the most
novel piece in that codebase.

## How it works

Three composable templates:

- **`Stash<T, Size>`** — the primitive: a chunked, lazily-grown array of
  `std::atomic<T*>`. Slots beyond `Size` spill into a linked node; chunks and
  nodes are published with `compare_exchange`, so concurrent producers never
  block.
- **`StashSlots<T, Size, CurrentKey, Div, Mod, Ring>`** — a keyed level: a key
  maps to slot `(key / Div) % Mod`, and `T` is itself a `Stash`, so levels
  **nest** to give multiple time resolutions. `next()` walks a key window in one
  of three modes — *walk* (consume), *peep* (look-ahead), *clean* (GC).
- **`StashValues<T, Size, CurrentKey>`** — the leaf: an append-only list with an
  atomic write cursor and separate walk/clean read cursors.

Nesting a few `StashSlots` over a `StashValues` gives a timer wheel. Xapiand's
scheduler stacks `50×1ms → 10×50ms → 36×500ms → 4800×18s` for a 24-hour horizon
at millisecond granularity.

## Usage

`stash` is a low-level building block; the intended consumer is a scheduler that
keys by a clock. A minimal single-level wheel storing values at integer keys:

```cpp
#include "stash.h"

static unsigned long long now() { return 0; }   // your clock / key source

using Item  = std::shared_ptr<int>;              // values must be pointer-like
using Leaf  = StashValues<Item, 4, &now>;
using Wheel = StashSlots<Leaf, 8, &now, /*Div*/1, /*Mod*/8, /*Ring*/true>;

Wheel wheel;
StashContext ctx(0ULL);
wheel.add(ctx, 1, std::make_shared<int>(11));    // insert at key 1
wheel.add(ctx, 2, std::make_shared<int>(22));

ctx.op = StashContext::Operation::walk;           // then walk what's "due"
ctx.begin_key = ctx.atom_first_valid_key.load();
ctx.end_key = 6;
Item item;
while (wheel.next(ctx, &item)) { /* item in key order */ ctx.begin_key = ctx.atom_first_valid_key.load(); }
```

See `test/test.cc` for a complete, runnable example. For the full timer-wheel /
scheduler design this was lifted from, see
[Xapiand's SCHEDULER.md](https://github.com/Kronuz/Xapiand/blob/master/SCHEDULER.md).

## Build & test

Header-only (`stash.h` + a tiny `stash_trace.h` of no-op trace stubs). To run the
smoke test:

```sh
c++ -std=c++17 -I. test/test.cc -o test/test && ./test/test
# or: cmake -B build && cmake --build build && ctest --test-dir build
```

Requires C++17.

## Notes

- **Value type must be pointer-like** (bool-testable and dereferenceable, e.g.
  `std::shared_ptr<T>`) — the walk tests `*ptr && **ptr`. Don't store a value
  that is "zero/empty" as a live entry; it reads as absent.
- **Standalone changes from the Xapiand original:** the logging include is
  replaced by `stash_trace.h` (empty `L_*` stubs — define your own before
  including to plug in tracing), and the debug-only color helper now returns `""`.
- **Vestigial template parameters:** `CurrentKey` and `Ring` are carried for
  source-compatibility with the original but are unused in the bodies; they can
  be dropped in a future cleanup.
- Single-consumer on the walk/clean side; many-producer on `add`/`put`.

## License

MIT — see [LICENSE](LICENSE).
