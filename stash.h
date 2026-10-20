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
#include <stdexcept>             // for std::out_of_range
#include <type_traits>           // for std::void_t

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


// Per-insert announce statistics, a no-op unless defined before this header:
// `probes` slots were tried past the hinted one (0 = the hint was free), and
// `overflowed` is true when none was and the insert took the overflow counter.
#ifndef STASH_ANNOUNCE_STAT
#define STASH_ANNOUNCE_STAT(probes, overflowed)
#endif


// Each thread's preferred announce slot; it moves to wherever the thread last
// found a free one.
inline unsigned& stash_announce_hint() noexcept {
	static std::atomic<unsigned> seq{0};
	static thread_local unsigned hint = seq.fetch_add(1, std::memory_order_relaxed);
	return hint;
}


struct StashContext {
	enum class Operation : uint8_t {
		walk,
		peep,
		clean,
	};

	// Sentinel for pending_floor and idle announce slots.
	static constexpr unsigned long long IDLE = std::numeric_limits<unsigned long long>::max();

	// Announce slots: more producers in flight at once than this share one
	// overflow counter, which holds first_valid still until they finish.
	static constexpr size_t ANNOUNCE_SLOTS = 64;

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

	// The descent-window handshake (ARCHITECTURE.md, "Inserts in flight").
	// add() announces its key in a slot before it touches the tree and files a
	// key below atom_insert_floor at that floor; the walk publishes the floor,
	// then reads the slots, and never raises first_valid above what it read.
	struct alignas(64) Announce {
		std::atomic_ullong key{IDLE};
	};
	std::array<Announce, ANNOUNCE_SLOTS> announce;
	alignas(64) std::atomic_ullong atom_announce_overflow{0};
	alignas(64) std::atomic_ullong atom_insert_floor;

	// Consumer-only: the floor last published, the first_valid ceiling read with
	// it, and the walk's recursion depth (the floor is refreshed at depth 0).
	unsigned long long insert_floor;
	unsigned long long first_valid_cap;
	unsigned depth = 0;

	StashContext(StashContext&& o) noexcept
		: op(std::move(o.op)),
		  begin_key(std::move(o.begin_key)),
		  end_key(std::move(o.end_key)),
		  atom_first_valid_key(o.atom_first_valid_key.load()),
		  atom_last_valid_key(o.atom_last_valid_key.load()),
		  horizon_margin(o.horizon_margin),
		  pending_floor(o.pending_floor),
		  atom_insert_floor(o.atom_insert_floor.load()),
		  insert_floor(o.insert_floor),
		  first_valid_cap(o.first_valid_cap) { }

	explicit StashContext(unsigned long long begin_key)
		: op(Operation::walk),
		  begin_key(begin_key),
		  end_key(begin_key),
		  atom_first_valid_key(begin_key),
		  atom_last_valid_key(begin_key),
		  atom_insert_floor(begin_key),
		  insert_floor(begin_key),
		  first_valid_cap(begin_key) { }

	// Producer side: an insert in flight. Announces before the descent and yields
	// the key to file under (never below the insert floor); the destructor
	// retracts the announce once the value is in place.
	class InFlight {
		std::atomic_ullong* slot = nullptr;
		std::atomic_ullong* overflow = nullptr;
		unsigned long long key_;

	public:
		InFlight(StashContext& ctx, unsigned long long key) {
			// Announce at most the key it will file under, so the walk may advance
			// to the current floor even while a late insert is in flight.
			auto floor = ctx.atom_insert_floor.load(std::memory_order_relaxed);
			auto announced = key < floor ? floor : key;
			auto& hint = stash_announce_hint();
			size_t i = 0;
			for (; i < ANNOUNCE_SLOTS; ++i) {
				auto idx = (hint + i) % ANNOUNCE_SLOTS;
				auto& a = ctx.announce[idx].key;
				auto idle = IDLE;
				// seq_cst: pairs with the walk's seq_cst floor store and slot loads
				// (a store-load handshake, so acq/rel is not enough).
				if (a.load(std::memory_order_relaxed) == IDLE &&
				    a.compare_exchange_strong(idle, announced,
				        std::memory_order_seq_cst, std::memory_order_relaxed)) {
					slot = &a;
					hint = static_cast<unsigned>(idx);
					break;
				}
			}
			if (!slot) {
				overflow = &ctx.atom_announce_overflow;
				overflow->fetch_add(1, std::memory_order_seq_cst);
			}
			STASH_ANNOUNCE_STAT(i, !slot);
			// Either the walk saw the announce, or this load sees its floor.
			floor = ctx.atom_insert_floor.load(std::memory_order_seq_cst);
			key_ = key < floor ? floor : key;
		}

		~InFlight() {
			// release: a walk that reads the slot idle sees the filled value.
			if (slot) {
				slot->store(IDLE, std::memory_order_release);
			} else {
				overflow->fetch_sub(1, std::memory_order_release);
			}
		}

		InFlight(const InFlight&) = delete;
		InFlight& operator=(const InFlight&) = delete;

		unsigned long long key() const noexcept {
			return key_;
		}
	};

	// Consumer side, at the start of a walk: publish the insert floor (end_key - 1)
	// once it has moved a finest slot, then read the announces. Until the next
	// refresh the walk keeps first_valid at or below the lowest announce, the
	// floor, and (with overflowed producers in flight) where it was.
	void refresh_first_valid_cap(unsigned long long period) {
		// Only where a walk starts (at first_valid): a snapshot must predate every
		// read of the walk whose first_valid it bounds, not just this call's.
		if (op != Operation::walk || !end_key ||
		    begin_key > atom_first_valid_key.load(std::memory_order_relaxed)) {
			return;
		}
		auto floor = end_key - 1;
		bool advanced = floor > insert_floor && floor - insert_floor >= period;
		bool pinned = first_valid_cap < insert_floor;
		if (!advanced && !pinned) {
			return;
		}
		if (floor > insert_floor) {
			insert_floor = floor;
			atom_insert_floor.store(floor, std::memory_order_seq_cst);
		}
		auto cap = insert_floor;
		for (auto& a : announce) {
			auto k = a.key.load(std::memory_order_seq_cst);
			if (k < cap) {
				cap = k;
			}
		}
		if (atom_announce_overflow.load(std::memory_order_seq_cst)) {
			auto f = atom_first_valid_key.load(std::memory_order_relaxed);
			if (f < cap) {
				cap = f;
			}
		}
		first_valid_cap = cap;
	}

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


// The finest slot width under a level: its own _Div over a leaf, else its child's.
template <typename T, typename = void>
struct stash_finest_div : std::integral_constant<unsigned long long, 0> { };

template <typename T>
struct stash_finest_div<T, std::void_t<decltype(T::finest_div)>>
	: std::integral_constant<unsigned long long, T::finest_div> { };


template <typename _Tp, size_t _Size, unsigned long long _Div, unsigned long long _Mod>
class StashSlots : public Stash<_Tp, _Size> {
	using Stash_T = Stash<_Tp, _Size>;

public:
	static constexpr unsigned long long finest_div =
		stash_finest_div<_Tp>::value ? stash_finest_div<_Tp>::value : _Div;

private:

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

		// The outermost level refreshes the walk's first_valid ceiling before
		// reading anything, so the ceiling predates every load this walk makes.
		struct Depth {
			unsigned& d;
			explicit Depth(unsigned& d) : d(d) { ++d; }
			~Depth() { --d; }
		} depth(ctx.depth);
		if (ctx.depth == 1) {
			ctx.refresh_first_valid_cap(finest_div);
		}

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
			// Nor past an insert that may still be descending (see InFlight).
			if (ctx.op == StashContext::Operation::walk && ctx.first_valid_cap < new_first_valid_key) {
				new_first_valid_key = ctx.first_valid_cap;
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
		// Announce before touching the tree; a key already below the insert floor
		// (overdue past the last walk's end) is filed at the floor instead.
		StashContext::InFlight in_flight(ctx, key);
		key = in_flight.key();

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

		L_STASH("StashSlots::" + LIGHT_PURPLE + "ADD" + CLEAR_COLOR + " - _Mod:{}, key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}", _Mod, key, ctx.atom_first_valid_key.load(), ctx.atom_last_valid_key.load());
		put(ctx, key, std::forward<Args>(args)...);
	}
};


template <typename _Tp, size_t _Size>
class StashValues : public Stash<_Tp, _Size, true> {
	using Stash_T = Stash<_Tp, _Size, true>;

	size_t walk_cur;                 // resume frontier: parks on the first reserved
	                                 // (sentinel) slot. Everything below it is drained
	                                 // (null); the walk drains ready values beyond it
	                                 // but never steps it past an unfilled hole, so the
	                                 // leaf stays pinned until the hole fills.
	size_t clean_cur;
	std::atomic_size_t atom_end;     // producer reserve frontier (next slot to claim).
	                                 // A slot is reserved (sentinel), filled (a value),
	                                 // or drained (null); the walk reads which directly.
	typename Stash_T::Cursor walk_cursor;    // consumer scan resume points (single
	typename Stash_T::Cursor clean_cursor;   // consumer; each tracks its own cur)

public:
	StashValues(StashValues&& o) noexcept
		: Stash_T::Stash(std::move(o)),
		  walk_cur(std::move(o.walk_cur)),
		  clean_cur(std::move(o.clean_cur)),
		  atom_end(o.atom_end.load()) { }

	StashValues()
		: walk_cur(0),
		  clean_cur(0),
		  atom_end(0) { }

	template <typename T>
	bool next(StashContext& ctx, T* value_ptr, unsigned long long) {
		const auto sentinel = stash_reserved<_Tp>();
		const bool is_clean = (ctx.op == StashContext::Operation::clean);
		const bool is_walk = (ctx.op == StashContext::Operation::walk);
		const bool is_peep = (ctx.op == StashContext::Operation::peep);

		// Resume the chain walk from this op's cursor. peep is read-only lookahead
		// and must not perturb the persistent walk cursor, so it uses a throwaway.
		typename Stash_T::Cursor peep_cursor;
		auto& cursor = is_walk  ? walk_cursor
		             : is_clean ? clean_cursor
		             :            peep_cursor;

		if (is_clean) {
			// Sweep the drained prefix [clean_cur, walk_cur): every slot there is
			// null (the walk drained it), so just advance the reclaim cursor.
			while (clean_cur < walk_cur) {
				std::atomic<_Tp*>* p = nullptr;
				if (Stash_T::get(&p, clean_cur, false, cursor) != StashState::Ok) {
					break;
				}
				++clean_cur;
			}
			return false;
		}

		// RMW read of the reserve frontier (fetch_add 0), not a plain load: an RMW is
		// serialized in atom_end's modification order, so it reads the latest count
		// and never a coherence-stale one. A stale bound would let the walk miss a
		// reserved slot and reclaim the leaf out from under a producer.
		const auto end = atom_end.fetch_add(0, std::memory_order_acquire);

		// Scan from the resume cursor. Drain ready values and skip drained (null)
		// slots, but PARK walk_cur on the first reserved hole and never step it past
		// one -- that hole is where the next walk resumes, and it keeps the leaf
		// pinned until the producer fills it. Ready values BEYOND the hole are still
		// drained, so one slow producer cannot stall the rest of the leaf.
		size_t cur = walk_cur;
		bool parked = false;
		while (cur < end) {
			std::atomic<_Tp*>* ptr_atom_ptr = nullptr;
			if (Stash_T::get(&ptr_atom_ptr, cur, false, cursor) != StashState::Ok) {
				// Reserved but not yet materialized: a hole. Park here and stop;
				// nothing beyond it can be materialized either.
				parked = true;
				break;
			}

			auto& atom_ptr = *ptr_atom_ptr;
			auto ptr = atom_ptr.load(std::memory_order_acquire);

			if (ptr == sentinel) {
				// A reserved hole: park walk_cur on the first one, but keep scanning
				// past it for ready values.
				parked = true;   // park walk_cur on the first hole
				++cur;
				continue;
			}

			if (ptr == nullptr) {
				// Already drained: skip. Advance the parked frontier only while it is
				// still contiguous (we have not yet hit a hole).
				if (!parked && is_walk) { walk_cur = cur + 1; }
				++cur;
				continue;
			}

			// A ready value.
			if (is_peep) {
				// Lookahead: report the value without taking it or moving the cursor.
				if (*ptr && **ptr) {
					if (value_ptr) { *value_ptr = *ptr; }
					return true;
				}
				++cur;
				continue;
			}

			// walk: take the value with an atomic exchange (not a plain store) so a
			// concurrent reclaim cannot grab the same pointer and double-free it; only
			// the winner of the exchange derefs and deletes it.
			if (!parked) { walk_cur = cur + 1; }   // advance the contiguous front
			ptr = atom_ptr.exchange(nullptr, std::memory_order_acquire);
			if (ptr == nullptr || ptr == sentinel) {
				++cur;   // lost the take, or not a real value
				continue;
			}
			if (*ptr && **ptr) {
				if (value_ptr) { *value_ptr = *ptr; }
				delete ptr;
				return true;
			}
			delete ptr;
			++cur;
		}

		// The leaf is pending if walk_cur parked on a hole (a reserved-not-yet-filled
		// slot) or producers reserved more slots after the scan began: undrained
		// slots remain below the reserve frontier, so the wheel must keep the leaf
		// pinned (capping first_valid) until the next walk drains them.
		if (is_walk && (parked || walk_cur < atom_end.fetch_add(0, std::memory_order_acquire))) {
			if (ctx.begin_key < ctx.pending_floor) {
				ctx.pending_floor = ctx.begin_key;
			}
		}

		return false;
	}

	template<typename... Args>
	void put(StashContext& ctx, unsigned long long key, Args&&... args) {
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
		// `key`, so a walk can descend to this leaf while the slot is still a
		// reserved hole and see it pending. first_valid is the walk's alone:
		// add() files keys at or above the insert floor, which first_valid never
		// passes. release on success: acquiring the bound (check()) orders the
		// reader after the path built during descent and after this reserve.
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
			}  // else: unique_ptr frees the loser (exception-safe)
		}
		// No commit step and no extra publish: the FILL's release CAS publishes the
		// value, and the consumer reads atom_end with an RMW (always fresh) plus each
		// slot with acquire, so it sees both the reservation and the value.
	}
};


#ifdef L_STASH_DEFINED
#undef L_STASH_DEFINED
#undef L_STASH
#endif
