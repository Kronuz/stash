/*
 * Example trace header for `stash`.
 *
 * Including this (via -DSTASH_TRACE_HEADER='"trace.h"') turns stash's no-op
 * tracing hooks into real, colored, std::format-rendered output, demonstrating
 * that the colored debug tracing Xapiand relies on is fully recoverable by a
 * consumer without modifying stash.h. A real consumer (like Xapiand) would
 * point these hooks at its own logger and color palette instead.
 *
 * Requires C++20 (for std::format / std::vformat).
 */

#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// --- ANSI color constants used inside stash.h's trace messages ---
// stash.h builds trace strings like  "StashSlots::" + CYAN + "LOOP" + CLEAR_COLOR
// so each token must concatenate with string literals; std::string does.
inline const std::string CLEAR_COLOR    = "\033[0m";
inline const std::string CYAN           = "\033[36m";
inline const std::string PURPLE         = "\033[35m";
inline const std::string FOREST_GREEN   = "\033[32m";
inline const std::string LIGHT_GREEN    = "\033[92m";
inline const std::string LIGHT_PURPLE   = "\033[95m";
inline const std::string LIGHT_RED      = "\033[91m";
inline const std::string LIGHT_SKY_BLUE = "\033[94m";
inline const std::string SADDLE_BROWN   = "\033[33m";

// --- per-operation color hook ---
// The StashContext::Operation enum is defined inside stash.h (which includes
// this header first), so accept it generically and switch on its value.
template <typename Op>
inline const char* stash_op_color(Op op) {
	switch (static_cast<int>(op)) {
		case 0:  return "\033[0m";   // walk  -> clear
		case 1:  return "\033[90m";  // peep  -> dim grey
		case 2:  return "\033[35m";  // clean -> purple
		default: return "\033[0m";
	}
}
#define STASH_OP_COLOR(op) stash_op_color(op)

// --- logging macros ---
// The format string is built at runtime (from the color concatenations above),
// so render with std::vformat. Args are materialized into a tuple as lvalues so
// std::make_format_args accepts them; on any format mismatch we fall back to the
// raw string rather than throwing out of a trace point.
namespace stash_example {
template <typename... Args>
inline std::string render(std::string_view f, Args&&... args) {
	try {
		auto store = std::make_tuple(std::forward<Args>(args)...);
		return std::apply(
			[&](auto&... a) { return std::vformat(f, std::make_format_args(a...)); },
			store);
	} catch (...) {
		return std::string(f);
	}
}
}  // namespace stash_example

#define L_STASH(...)             std::puts((stash_example::render(__VA_ARGS__) + "\033[0m").c_str())
#define L_DEBUG_HOOK(label, ...) std::puts((stash_example::render(__VA_ARGS__) + "\033[0m").c_str())
#define L_EXC(...)               std::puts(stash_example::render(__VA_ARGS__).c_str())
