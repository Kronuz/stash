/*
 * No-op trace stubs for the standalone `stash` library.
 *
 * Xapiand's `stash.h` traces through its logging macros (`L_STASH`,
 * `L_DEBUG_HOOK`, `L_EXC`), which by default compile to nothing. This header
 * provides empty definitions so the data structure builds with no dependency on
 * Xapiand's logging/color headers.
 *
 * To plug in real tracing, define these macros before including `stash.h`.
 */

#pragma once

#ifndef L_NOTHING
#define L_NOTHING(...)
#endif

#ifndef L_DEBUG_HOOK
#define L_DEBUG_HOOK(...)
#endif

#ifndef L_EXC
#define L_EXC(...)
#endif
