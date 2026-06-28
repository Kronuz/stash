/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <array>                 // for std::array
#include <atomic>                // for std::atomic
#include <cassert>               // for assert
#include <cstdint>               // for uintptr_t
#include <limits>                // for std::numeric_limits
#include <memory>                // for std::unique_ptr / std::make_unique

// Tracing and coloring are optional and fully injectable. Define
// STASH_TRACE_HEADER (a header path) before including this file to plug in your
// own logging macros (L_STASH / L_DEBUG_HOOK / L_EXC) and per-operation colors
// (STASH_OP_COLOR); otherwise the bundled no-op stubs are used. See
// stash_trace.h and examples/colored_trace/ for a working override.
#ifdef STASH_TRACE_HEADER
#  include STASH_TRACE_HEADER
#else
#  include "stash_trace.h"
#endif


// #define L_STASH L_COLLECT
#ifndef L_STASH
#define L_STASH_DEFINED
#define L_STASH L_NOTHING
#endif


enum class StashState : uint8_t {
	Ok,
	ChunkEmpty,
	StashShort,
	StashEmpty,
};


// A leaf slot reserved by a producer but not yet filled holds this sentinel,
// distinct from null (drained) and from a real value pointer. It is never
// dereferenced; the walk stops on it instead of stepping past the hole.
template <typename T>
inline T* stash_reserved() noexcept {
	return reinterpret_cast<T*>(~static_cast<uintptr_t>(0));
}

#ifdef STASH_DBG
#include <cstdio>
inline std::atomic<long> dbg_fill_ok{0}, dbg_fill_skip{0}, dbg_fire{0}, dbg_drain_invalid{0},
	dbg_stop_sentinel{0}, dbg_stop_notok{0}, dbg_skip_null{0}, dbg_pending{0}, dbg_reclaim{0};
__attribute__((destructor)) static void dbg_print() {
	fprintf(stderr, "[DBG] fill_ok=%ld fill_skip=%ld fire=%ld drain_invalid=%ld stop_sentinel=%ld stop_notok=%ld skip_null=%ld pending=%ld reclaim_undrained=%ld\n",
		dbg_fill_ok.load(), dbg_fill_skip.load(), dbg_fire.load(), dbg_drain_invalid.load(),
		dbg_stop_sentinel.load(), dbg_stop_notok.load(), dbg_skip_null.load(), dbg_pending.load(), dbg_reclaim.load());
}
#define DBG(x) (x).fetch_add(1, std::memory_order_relaxed)
#else
#define DBG(x) ((void)0)
#endif


struct StashContext {
	enum class Operation : uint8_t {
		walk,
		peep,
		clean,
	};

	// Sentinel for pending_floor: "no in-flight insert seen this walk".
	static constexpr unsigned long long IDLE = std::numeric_limits<unsigned long long>::max();

	Operation op;

	unsigned long long begin_key;
	unsigned long long end_key;

	std::atomic_ullong atom_first_valid_key;
	std::atomic_ullong atom_last_valid_key;

	// R2: keep-out zone at the far end of the wheel. add() rejects keys within
	// horizon_margin of the horizon, so a near-horizon insert can never alias onto
	// a physical slot the consumer is reclaiming one period below. 0 = off (the
	// full span is schedulable, pre-R2 behavior). Set it >= the consumer's clean
	// margin to close the aliasing window.
	unsigned long long horizon_margin = 0;

	// Step 3: lowest key of a leaf seen *pending* (reserved-not-written) during a
	// walk. The wheel caps its first_valid advance at this key, so reclamation
	// never crosses a leaf with an in-flight insert -- the held-back tail behind a
	// stalled producer is re-drained next pass instead of being dropped. Reset to
	// IDLE by the consumer at the start of each walk; only ever lowered, by the
	// leaf, during that walk.
	unsigned long long pending_floor = IDLE;

	StashContext(StashContext&& o) noexcept
		: op(std::move(o.op)),
		  begin_key(std::move(o.begin_key)),
		  end_key(std::move(o.end_key)),
		  atom_first_valid_key(o.atom_first_valid_key.load()),
		  atom_last_valid_key(o.atom_last_valid_key.load()),
		  horizon_margin(o.horizon_margin),
		  pending_floor(o.pending_floor) { }

	explicit StashContext(unsigned long long begin_key)
		: op(Operation::walk),
		  begin_key(begin_key),
		  end_key(begin_key),
		  atom_first_valid_key(begin_key),
		  atom_last_valid_key(begin_key) { }

	bool check(unsigned long long key, unsigned long long limit_key) const {
		if (end_key && key >= end_key) {
			return false;
		}

		if (limit_key && key >= limit_key) {
			return false;
		}

		// acquire: the producer widens atom_last_valid_key (release) only after the
		// path to `key` is built, so acquiring it here orders our descent after that
		// path's publication -- we never chase a key whose nodes we can't yet see.
		if (key > atom_last_valid_key.load(std::memory_order_acquire)) {
			return false;
		}

		return true;
	}

	const char* _op() const noexcept {
		switch (op) {
			case Operation::walk:
				return "walk";
			case Operation::peep:
				return "peep";
			case Operation::clean:
				return "clean";
			default:
				return "unknown";
		}
	}

	// Per-operation color for trace lines. Resolved through the STASH_OP_COLOR
	// hook, which is "" by default (no color) and overridable by a trace header
	// to restore colored output. Only ever called from the trace macros.
	const char* _col() const noexcept {
		return STASH_OP_COLOR(op);
	}
};


template <typename _Tp, size_t _Size, bool _Reserved = false>
class Stash {
protected:
	class Data;

	// Sequential-access cursor: a single consumer reading slots in order can resume
	// the chain walk from the node it last resolved instead of re-walking from the
	// head every call. Consumer-private (never shared with producers); a leaf's
	// chain only grows during its life, so the cached node is never freed under it.
	struct Cursor {
		Data* node = nullptr;   // chunk node covering [base, base + _Size)
		size_t base = 0;        // first slot index of that node's chunk
	};

	class Data {
		using Chunks = std::array<std::atomic<_Tp*>, _Size>;
		std::atomic<Chunks*> atom_chunk;
		std::atomic<Data*> atom_next;

	public:
		Data()
			: atom_chunk(nullptr),
			  atom_next(nullptr) { }

		// Move and destroy happen single-threaded (construction / teardown of an
		// unshared node), so these atomics need no inter-thread ordering: relaxed.
		Data(Data&& o) noexcept
			: atom_chunk(o.atom_chunk.load(std::memory_order_relaxed)),
			  atom_next(o.atom_next.load(std::memory_order_relaxed)) { }

		~Data() noexcept {
			try {
				auto next = atom_next.exchange(nullptr, std::memory_order_relaxed);
				while (next) {
					auto next_next = next->atom_next.exchange(nullptr, std::memory_order_relaxed);
					delete next;
					next = next_next;
				}

				auto chunk = atom_chunk.exchange(nullptr, std::memory_order_relaxed);
				if (chunk) {
					for (auto& atom_ptr : *chunk) {
						auto ptr = atom_ptr.exchange(nullptr, std::memory_order_relaxed);
						if (ptr && ptr != stash_reserved<_Tp>()) {
							if constexpr (_Reserved) { DBG(dbg_reclaim); }
							delete ptr;
						}
					}
					delete chunk;
				}
			} catch (...) {
				L_EXC("Unhandled exception in destructor");
			}
		}

		StashState get(std::atomic<_Tp*>** pptr_atom_ptr, size_t slot, bool spawn,
		               Cursor* cursor = nullptr) {
			if (!spawn &&
			    !atom_next.load(std::memory_order_acquire) &&
			    !atom_chunk.load(std::memory_order_acquire)) {
				return StashState::StashEmpty;
			}

			size_t chunk_num = slot / _Size;
			size_t local = slot % _Size;

			// Walk the chain to the chunk holding `slot`. A single consumer reading
			// slots in order would otherwise re-walk from the head every call
			// (O(slot/_Size)); a cursor lets it resume from the last node it
			// resolved, making sequential access O(1) amortized. The cursor is only
			// usable when it sits at or before the target chunk (a backward access,
			// or none, starts from the head). It is consumer-private -- never shared
			// with producers -- and a leaf's chain only grows during its life, so the
			// cached node is never freed while the cursor holds it.
			auto _data = this;
			size_t c = 0;
			if (cursor && cursor->node && cursor->base / _Size <= chunk_num) {
				_data = cursor->node;
				c = cursor->base / _Size;
			}

			for (; c < chunk_num; ++c) {
				// acquire: a non-null next must carry the producer's release of the
				// node it published (below), so we see that node fully constructed.
				auto next = _data->atom_next.load(std::memory_order_acquire);
				if (!next) {
					if (!spawn) {
						if (cursor) { cursor->node = _data; cursor->base = c * _Size; }
						return StashState::StashShort;
					}
					auto tmp = std::make_unique<Data>();
					// release on success publishes the new node; acquire on failure
					// loads the winner so we then read its contents safely.
					if (_data->atom_next.compare_exchange_strong(next, tmp.get(),
					        std::memory_order_release, std::memory_order_acquire)) {
						next = tmp.release();
					}  // else: unique_ptr frees the loser (exception-safe)
				}
				_data = next;
			}

			if (cursor) { cursor->node = _data; cursor->base = chunk_num * _Size; }

			auto chunk = _data->atom_chunk.load(std::memory_order_acquire);
			if (!chunk) {
				if (!spawn) {
					return StashState::ChunkEmpty;
				}
				auto tmp = std::make_unique<Chunks>();
				if constexpr (_Reserved) {
					// Mark every slot reserved before publishing the array, so a
					// consumer that acquires the chunk can tell a not-yet-filled slot
					// (sentinel) from a drained one (null). relaxed: these stores are
					// ordered before the array by the release-CAS that publishes it.
					for (auto& a : *tmp) {
						a.store(stash_reserved<_Tp>(), std::memory_order_relaxed);
					}
				}
				// release publishes the new chunk array; acquire on failure loads the
				// winner so the slots we hand out are the published ones.
				if (_data->atom_chunk.compare_exchange_strong(chunk, tmp.get(),
				        std::memory_order_release, std::memory_order_acquire)) {
					chunk = tmp.release();
				}  // else: unique_ptr frees the loser (exception-safe)
			}

			auto& atom_ptr = (*chunk)[local];

			*pptr_atom_ptr = &atom_ptr;
			return StashState::Ok;
		}
	};

	Data data;

public:
	Stash(Stash&& o) noexcept
		: data(std::move(o.data)) { }

	Stash() = default;

	StashState get(std::atomic<_Tp*>** pptr_atom_ptr, size_t slot, bool spawn) {
		/* If spawn is false, get() could fail with:
		 *   StashState::StashEmpty
		 *   StashState::ChunkEmpty
		 *   StashState::StashShort
		 */
		return data.get(pptr_atom_ptr, slot, spawn);
	}

protected:
	// Cursor-aware get: a single consumer scanning slots in order passes its own
	// Cursor so the chain walk resumes instead of restarting from the head.
	StashState get(std::atomic<_Tp*>** pptr_atom_ptr, size_t slot, bool spawn, Cursor& cursor) {
		return data.get(pptr_atom_ptr, slot, spawn, &cursor);
	}
};


template <typename _Tp, size_t _Size, unsigned long long _Div, unsigned long long _Mod>
class StashSlots : public Stash<_Tp, _Size> {
	using Stash_T = Stash<_Tp, _Size>;

	unsigned long long get_base_key(unsigned long long key) const {
		return (key / _Div) * _Div;
	}

	unsigned long long get_inc_base_key(unsigned long long key) const {
		return get_base_key(key) + _Div;
	}

	unsigned long long get_dec_base_key(unsigned long long key) const {
		// Start of the previous slot. Guard against unsigned underflow at the
		// low end of the key space: there is no slot below 0, so floor at 0
		// instead of wrapping to ~2^64. The wrapped value would otherwise be
		// published into atom_first_valid_key by the CAS loop in next() and
		// corrupt the bound.
		auto base_key = get_base_key(key);
		if (base_key < _Div) {
			return 0;
		}
		return base_key - _Div;
	}

	unsigned long long get_end_base_key(unsigned long long key) const {
		// One wheel span past the slot's base, used as the overflow horizon.
		// Guard against unsigned overflow for clock-derived keys high in the
		// 64-bit range: if base_key + span would wrap past the max value, the
		// horizon is effectively unbounded, so saturate at the max instead of
		// wrapping to a small value (which would make the overflow guard reject
		// valid keys or accept invalid ones).
		constexpr auto max_key = std::numeric_limits<unsigned long long>::max();
		constexpr auto span = _Div * _Mod;
		auto base_key = get_base_key(key);
		if (base_key > max_key - span) {
			return max_key;
		}
		return base_key + span;
	}

	size_t get_slot(unsigned long long key) const {
		return (key / _Div) % _Mod;
	}

public:
	StashSlots(StashSlots&& o) noexcept
		: Stash_T::Stash(std::move(o)) { }

	StashSlots() = default;

	template <typename T>
	bool next(StashContext& ctx, T* value_ptr, unsigned long long limit_key) {
		bool found = false;

		auto loop = ctx.check(ctx.begin_key, limit_key);

		while (loop) {
			auto new_first_valid_key = get_inc_base_key(ctx.begin_key);
			auto cur = get_slot(ctx.begin_key);

			L_DEBUG_HOOK("StashSlots::LOOP", "StashSlots::" + CYAN + "LOOP" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, cur, limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());

			std::atomic<_Tp*>* ptr_atom_ptr = nullptr;
			switch (Stash_T::get(&ptr_atom_ptr, cur, false)) {
				case StashState::Ok:
					break;
				case StashState::ChunkEmpty:
					L_STASH("StashSlots::" + SADDLE_BROWN + "EMPTY" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, cur, limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());
					break;
				case StashState::StashShort:
				case StashState::StashEmpty:
					L_STASH("StashSlots::" + SADDLE_BROWN + "BREAK" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, cur, limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());
					goto ret_next;
			}

			if (ptr_atom_ptr) {
				auto& atom_ptr = *ptr_atom_ptr;
				auto ptr = atom_ptr.load(std::memory_order_acquire);   // see the published child
				if (ptr) {
					if (ctx.op == StashContext::Operation::clean) {
						// Reclaim: once a slot's whole window is behind the safe
						// cutoff (ctx.end_key), the single walker is provably past it
						// and no producer can write the past, so the whole subtree can
						// be dropped in one shot with no further checks or locks.
						// new_first_valid_key is this slot's upper bound; only drop
						// slots entirely behind the cutoff, never the straddling one.
						if (new_first_valid_key <= ctx.end_key) {
							// acquire: own the subtree fully before deleting it (the
							// store of null publishes nothing, so no release needed).
							ptr = atom_ptr.exchange(nullptr, std::memory_order_acquire);
							if (ptr) {
								L_STASH("StashSlots::" + LIGHT_RED + "CLEAR" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, cur, limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());
								delete ptr;
							}
						}
					} else {
						auto status = ptr->next(ctx, value_ptr, new_first_valid_key);
						if (status) {
							L_STASH("StashSlots::" + FOREST_GREEN + "FOUND" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, cur, limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());
							found = true;
							goto ret_next;
						}
					}
				}
			}

			loop = ctx.check(new_first_valid_key, limit_key);

			if (loop) {
				ctx.begin_key = new_first_valid_key;
			}
		}

		L_STASH("StashSlots::" + SADDLE_BROWN + "MISSING" + CLEAR_COLOR + " - {}_Mod:{}, begin_key:{}, end_key:{}, cur:{}, limit_key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.begin_key, ctx.end_key, get_slot(ctx.begin_key), limit_key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());

	ret_next:
		if (ctx.op != StashContext::Operation::peep) {
			if (!found) {
				unsigned long long new_cur_key;
				if (!limit_key || (ctx.end_key && ctx.end_key < limit_key)) {
					assert(ctx.end_key);
					new_cur_key = get_base_key(ctx.end_key);
				} else {
					new_cur_key = get_base_key(limit_key);
				}
				if (new_cur_key > ctx.begin_key) {
					ctx.begin_key = new_cur_key;
				}
			}
			auto new_first_valid_key = get_dec_base_key(ctx.begin_key);
			// Step 3: never advance the low-water mark past a leaf seen pending this
			// walk. Capping first_valid at the pending leaf's base keeps the next
			// walk descending to it (to drain the held-back tail behind a stalled
			// producer) and keeps clean -- seeded from first_valid -- from reclaiming
			// it. pending_floor is IDLE when nothing is pending (and stays IDLE for
			// the clean context), so this is a no-op in the common case.
			if (ctx.pending_floor != StashContext::IDLE) {
				auto pending_base = get_base_key(ctx.pending_floor);
				if (pending_base < new_first_valid_key) {
					new_first_valid_key = pending_base;
				}
			}
			// release on success publishes the advanced low-water mark; the loop's
			// reload only needs the latest value, so relaxed there.
			auto first_valid_key = ctx.atom_first_valid_key.load(std::memory_order_relaxed);
			while (new_first_valid_key > first_valid_key &&
			       !ctx.atom_first_valid_key.compare_exchange_weak(first_valid_key, new_first_valid_key,
			           std::memory_order_release, std::memory_order_relaxed));
		}

		return found;
	}

	template <typename T>
	bool next(StashContext& ctx, T* value_ptr) {
		return next(ctx, value_ptr, 0);
	}

	template<typename... Args>
	void put(StashContext& ctx, unsigned long long key, Args&&... args) {
		auto slot = get_slot(key);
		L_STASH("StashSlots::" + PURPLE + "PUT" + CLEAR_COLOR + " - {}_Mod:{}, end_key:{}, key:{}, slot:{}, begin_key:{}, cur:{}, atom_first_valid_key:{}, atom_last_valid_key:{}, op:{}", ctx._col(), _Mod, ctx.end_key, key, slot, ctx.begin_key, get_slot(ctx.begin_key), ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load(), ctx._op());

		std::atomic<_Tp*>* ptr_atom_ptr;
		Stash_T::get(&ptr_atom_ptr, slot, true);

		auto& atom_ptr = *ptr_atom_ptr;
		auto ptr = atom_ptr.load(std::memory_order_acquire);
		if (!ptr) {
			auto tmp = std::make_unique<_Tp>();
			// release publishes the new child subtree; acquire on failure loads the
			// winner so the recursive put() below descends into a visible node.
			if (atom_ptr.compare_exchange_strong(ptr, tmp.get(),
			        std::memory_order_release, std::memory_order_acquire)) {
				ptr = tmp.release();
			}  // else: unique_ptr frees the loser (exception-safe)
		}

		ptr->put(ctx, key, std::forward<Args>(args)...);
	}

	template<typename... Args>
	void add(StashContext& ctx, unsigned long long key, Args&&... args) {
		// R2: the schedulable horizon, minus an optional keep-out zone. Rejecting
		// keys within horizon_margin of the end stops a near-horizon insert from
		// aliasing onto a slot being reclaimed one wheel period below.
		auto horizon = get_end_base_key(ctx.atom_first_valid_key.load(std::memory_order_acquire));
		if (horizon > ctx.horizon_margin) {
			horizon -= ctx.horizon_margin;
		}
		if (key >= horizon) {
			throw std::out_of_range("stash overlow");
		}

		// Step 2: the valid-key bound is now published inside the leaf's put(),
		// between RESERVE and FILL, so it covers `key` while the slot is still a
		// hole. It used to be widened here, after the whole descent+fill returned.
		L_STASH("StashSlots::" + LIGHT_PURPLE + "ADD" + CLEAR_COLOR + " - _Mod:{}, key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}", _Mod, key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load());
		put(ctx, key, std::forward<Args>(args)...);
	}
};


template <typename _Tp, size_t _Size>
class StashValues : public Stash<_Tp, _Size, true> {
	using Stash_T = Stash<_Tp, _Size, true>;

	size_t walk_cur;
	size_t clean_cur;
	std::atomic_size_t atom_end;     // producer reserve frontier (next slot to claim).
	                                 // A slot is reserved (sentinel), filled (a value),
	                                 // or drained (null); the walk reads which directly
	                                 // and stops at the first reserved slot, so there is
	                                 // no separate written frontier to maintain.
	std::atomic<long> atom_inflight; // producers currently between reserve and fill on
	                                 // this leaf; >0 means an insert is in flight, so the
	                                 // leaf must be pinned (it replaces atom_ready's lag).
	typename Stash_T::Cursor walk_cursor;    // consumer scan resume points (single
	typename Stash_T::Cursor clean_cursor;   // consumer; each tracks its own cur)

public:
	StashValues(StashValues&& o) noexcept
		: Stash_T::Stash(std::move(o)),
		  walk_cur(std::move(o.walk_cur)),
		  clean_cur(std::move(o.clean_cur)),
		  atom_end(o.atom_end.load()),
		  atom_inflight(o.atom_inflight.load()) { }

	StashValues()
		: walk_cur(0),
		  clean_cur(0),
		  atom_end(0),
		  atom_inflight(0) { }

	template <typename T>
	bool next(StashContext& ctx, T* value_ptr, unsigned long long) {
		const auto sentinel = stash_reserved<_Tp>();
		const bool is_clean = (ctx.op == StashContext::Operation::clean);
		const bool is_walk = (ctx.op == StashContext::Operation::walk);

		auto cur = is_clean ? clean_cur : walk_cur;

		// Resume the chain walk from this op's cursor. peep is read-only lookahead
		// and must not perturb the persistent walk cursor, so it uses a throwaway.
		typename Stash_T::Cursor peep_cursor;
		auto& cursor = is_walk  ? walk_cursor
		             : is_clean ? clean_cursor
		             :            peep_cursor;

		// The walk scans up to the reserve frontier, reading each slot's state
		// directly: a value is fired/drained, a drained (null) slot is skipped, and
		// the first reserved (sentinel) or not-yet-materialized slot stops the scan.
		// clean stays strictly behind the walk cursor, where every slot is drained.
		// RMW read (fetch_add 0), not a plain load: an RMW is serialized in atom_end's
		// modification order and so reads the *latest* reserve count, never a
		// coherence-stale one. A stale bound would let the walk miss filled slots and
		// reclaim the leaf out from under them. clean stays behind the walk cursor.
		auto end = is_clean ? walk_cur : atom_end.fetch_add(0, std::memory_order_acquire);

		while (cur < end) {
			std::atomic<_Tp*>* ptr_atom_ptr = nullptr;
			if (Stash_T::get(&ptr_atom_ptr, cur, false, cursor) != StashState::Ok) {
				// Reserved (atom_end passed this slot) but its storage is not
				// materialized yet: a hole. Stop, leaving the cursor parked here.
				L_STASH("StashValues::" + SADDLE_BROWN + "BREAK" + CLEAR_COLOR + " - {}cur:{}, atom_end:{}, op:{}", ctx._col(), cur, atom_end.load(), ctx._op());
				if (is_walk) { DBG(dbg_stop_notok); }
				break;
			}

			auto& atom_ptr = *ptr_atom_ptr;
			auto ptr = atom_ptr.load(std::memory_order_acquire);

			if (ptr == sentinel) {
				// Reserved but not yet filled: the first hole. Stop; the next walk
				// resumes here and re-reads it once the producer fills it.
				if (is_walk) { DBG(dbg_stop_sentinel); }
				break;
			}

			if (is_walk && ptr == nullptr) { DBG(dbg_skip_null); }

			bool returning = false;
			if (ptr != nullptr) {
				// A filled slot: fire its value (walk/peep) and drain it (walk/clean).
				if (!is_clean && *ptr && **ptr) {
					L_STASH("StashValues::" + LIGHT_GREEN + "FOUND" + CLEAR_COLOR + " - {}cur:{}, atom_end:{}, op:{}", ctx._col(), cur, atom_end.load(), ctx._op());
					if (value_ptr) {
						*value_ptr = *ptr;
					}
					returning = true;
					DBG(dbg_fire);
				} else if (is_walk) {
					DBG(dbg_drain_invalid);
				}
				if (ctx.op != StashContext::Operation::peep) {
					// acquire: own the value before firing/deleting it. Drain to null,
					// the drained state, distinct from the reserved sentinel.
					ptr = atom_ptr.exchange(nullptr, std::memory_order_acquire);
					if (ptr && ptr != sentinel) {
						L_STASH("StashValues::" + LIGHT_RED + "CLEAR" + CLEAR_COLOR + " - {}cur:{}, atom_end:{}, op:{}", ctx._col(), cur, atom_end.load(), ctx._op());
						delete ptr;
					}
				}
			}

			// Advance past this slot (a fired/drained value, or an already-drained
			// null). peep advances only its local cursor and never persists.
			++cur;
			if (is_walk) {
				walk_cur = cur;
			} else if (is_clean) {
				clean_cur = cur;
			}
			if (returning) {
				return true;
			}
		}

		// If the walk did not reach the current reserve frontier, this leaf is
		// *pending*: it stopped at a hole (a reserved-not-filled slot) or producers
		// reserved more slots after the scan started. Either way undrained slots
		// remain, so mark the leaf pending: the wheel refuses to advance first_valid
		// past it, and the held-back tail is re-drained next pass rather than
		// reclaimed out from under an in-flight producer. Re-load atom_end so a
		// reservation that landed during the scan is not missed. Walk only: peep is
		// read-only; clean runs behind walk_cur, where there are no holes.
		if (is_walk && (cur < atom_end.fetch_add(0, std::memory_order_acquire) ||
		                atom_inflight.fetch_add(0, std::memory_order_acquire) > 0)) {
			DBG(dbg_pending);
			if (ctx.begin_key < ctx.pending_floor) {
				ctx.pending_floor = ctx.begin_key;
			}
		}

		return false;
	}

	template<typename... Args>
	void put(StashContext& ctx, unsigned long long key, Args&&... args) {
		// Mark this insert in flight before reserving, so a consumer that observes
		// the count (acquire) pins the leaf for the whole reserve-to-fill window.
		atom_inflight.fetch_add(1, std::memory_order_acq_rel);

		// RESERVE: claim a slot and ensure its physical location exists. The path
		// to this leaf was already built during descent, so the consumer can reach
		// it the instant the bound below is published. relaxed: the slot index is
		// private, and this increment is made visible to the consumer's pending
		// check by the bound's release-store below (sequenced after it).
		auto slot = atom_end.fetch_add(1, std::memory_order_relaxed);
		L_STASH("StashValues::" + LIGHT_PURPLE + "PUT" + CLEAR_COLOR + " - {}slot:{}, atom_end:{}, op:{}", ctx._col(), slot, atom_end.load(), ctx._op());

		std::atomic<_Tp*>* ptr_atom_ptr;
		Stash_T::get(&ptr_atom_ptr, slot, true);

		// PUBLISH BOUND (before the fill): widen the valid-key window to cover
		// `key`. Doing this *before* the fill is the crux of Step 2 -- it lets the
		// walk descend to this leaf while the slot is still a hole and observe it
		// as pending (atom_ready < atom_end), so Step 3 can pin first_valid to it.
		// Publishing after the fill (where these two CAS loops used to live, at the
		// end of StashSlots::add) would hide the pending state and let the
		// consumer's low-water mark run past an in-flight insert.
		// release on success: the bound is the leaf's publication point. Acquiring
		// it (check(), ret_next, the R2 horizon) orders the reader after the path
		// built during descent and after this slot's reserve.
		auto first_valid_key = ctx.atom_first_valid_key.load(std::memory_order_relaxed);
		while (key < first_valid_key &&
		       !ctx.atom_first_valid_key.compare_exchange_weak(first_valid_key, key,
		           std::memory_order_release, std::memory_order_relaxed));
		auto last_valid_key = ctx.atom_last_valid_key.load(std::memory_order_relaxed);
		while (key > last_valid_key &&
		       !ctx.atom_last_valid_key.compare_exchange_weak(last_valid_key, key,
		           std::memory_order_release, std::memory_order_relaxed));

		// FILL: publish the value into the reserved slot. Only this producer writes
		// this slot (the reserve made it private), and the slot was initialized to the
		// reserved sentinel when its chunk was spawned. The CAS over the sentinel
		// releases so a consumer that later reads this slot sees the value fully
		// constructed. There is no commit step: the walk reads each slot's state
		// (sentinel / value / null) directly and stops at the first sentinel, so a
		// producer never advances a shared frontier on behalf of others.
		auto& atom_ptr = *ptr_atom_ptr;
		auto ptr = atom_ptr.load(std::memory_order_relaxed);
		if (ptr == stash_reserved<_Tp>()) {
			auto tmp = std::make_unique<_Tp>(std::forward<Args>(args)...);
			if (atom_ptr.compare_exchange_strong(ptr, tmp.get(),
			        std::memory_order_release, std::memory_order_relaxed)) {
				ptr = tmp.release();
				DBG(dbg_fill_ok);
			}  // else: unique_ptr frees the loser (exception-safe)
		} else {
			DBG(dbg_fill_skip);
		}

		// Publish: a release RMW on atom_end so the consumer's acquire-load of it
		// synchronizes-with this producer, seeing both the reservation and the value
		// behind it. This is the synchronization the removed COMMIT loop provided,
		// without the per-slot frontier walk.
		atom_end.fetch_add(0, std::memory_order_release);

		// Insert complete; release so a consumer that sees the count drop to zero
		// (acquire) also sees the filled value.
		atom_inflight.fetch_sub(1, std::memory_order_release);
	}
};


#ifdef L_STASH_DEFINED
#undef L_STASH_DEFINED
#undef L_STASH
#endif
