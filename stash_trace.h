/*
 * Default (no-op) tracing and coloring hooks for the standalone `stash` library.
 *
 * `stash.h` instruments itself through three logging macros (`L_STASH`,
 * `L_DEBUG_HOOK`, `L_EXC`) and one color hook (`STASH_OP_COLOR`). By default they
 * all compile to nothing, so the data structure builds with zero dependency on
 * any logging or color headers, and tracing has no runtime cost.
 *
 * To restore traced, colored output (the way Xapiand uses it), provide your own
 * versions of these. Two ways:
 *
 *   1. Define STASH_TRACE_HEADER to the path of a header that defines them, e.g.
 *        c++ -DSTASH_TRACE_HEADER='"my_trace.h"' ...
 *      `stash.h` will include that instead of this file.
 *
 *   2. Define the macros directly before including `stash.h`.
 *
 * Each macro is `#ifndef`-guarded, so defining any subset is fine; the rest fall
 * back to the no-op defaults here. See examples/colored_trace/ for a complete,
 * runnable override that produces colored, std::format-rendered trace lines.
 */

#pragma once

// Logging hooks. L_STASH is set to L_NOTHING by stash.h unless already defined;
// L_DEBUG_HOOK takes a label as its first argument, then a format + args.
#ifndef L_NOTHING
#define L_NOTHING(...)
#endif

#ifndef L_DEBUG_HOOK
#define L_DEBUG_HOOK(...)
#endif

#ifndef L_EXC
#define L_EXC(...)
#endif

// Color hook: given the current StashContext::Operation, return a C string (an
// ANSI escape) used to tint trace lines. No color by default. Override to
// colorize per operation. Must return a pointer that stays valid for the trace
// call (e.g. a string literal or a function-local static).
#ifndef STASH_OP_COLOR
#define STASH_OP_COLOR(op) ""
#endif
