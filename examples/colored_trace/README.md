# colored_trace example

A runnable demonstration that `stash`'s tracing and coloring are fully
injectable from outside the library, with no edit to `stash.h`.

`stash.h` is heavily instrumented, but every trace line flows through four hooks
that are no-ops by default: `L_STASH`, `L_DEBUG_HOOK`, `L_EXC`, and the
per-operation color hook `STASH_OP_COLOR`. This example provides real versions of
all four in `trace.h`:

- ANSI color constants (`CYAN`, `CLEAR_COLOR`, `FOREST_GREEN`, ...) that the
  trace strings inside `stash.h` concatenate into their messages.
- `STASH_OP_COLOR(op)`, mapping each `StashContext::Operation` (walk / peep /
  clean) to its own tint.
- the `L_*` macros, which render the runtime-built format strings with
  `std::vformat` and print them, so every `{}` is substituted.

`main.cc` then runs a small wheel (`StashValues` under a `StashSlots`), adds a
couple of values, and walks them.

## Build & run

```sh
c++ -std=c++20 -I. -I../.. -DSTASH_TRACE_HEADER='"trace.h"' main.cc -o demo && ./demo
```

`-DSTASH_TRACE_HEADER='"trace.h"'` tells `stash.h` to include `trace.h` instead
of the bundled no-op `stash_trace.h`. C++20 is required for `std::format` /
`std::vformat` (this example only; `stash.h` itself is C++17).

You should see colored, fully-formatted trace lines with per-operation colors and
inline word colors (`PUT`, `ADD`, `LOOP`, `FOUND`, `CLEAR`, ...). Drop the
`-DSTASH_TRACE_HEADER=...` flag and the exact same `stash.h` produces no trace
and no color at all.

## What a real consumer does

`trace.h` is a self-contained demo logger. A real consumer such as Xapiand points
the same four hooks at its own logger and color palette instead, recovering the
colored debug tracing it relies on without modifying `stash.h`.
