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


struct StashContext {
	enum class Operation : uint8_t {
		walk,
		peep,
		clean,
	};

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

	StashContext(StashContext&& o) noexcept
		: op(std::move(o.op)),
		  begin_key(std::move(o.begin_key)),
		  end_key(std::move(o.end_key)),
		  atom_first_valid_key(o.atom_first_valid_key.load()),
		  atom_last_valid_key(o.atom_last_valid_key.load()),
		  horizon_margin(o.horizon_margin) { }

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

		if (key > atom_last_valid_key.load()) {
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


template <typename _Tp, size_t _Size>
class Stash {
protected:
	class Data {
		using Chunks = std::array<std::atomic<_Tp*>, _Size>;
		std::atomic<Chunks*> atom_chunk;
		std::atomic<Data*> atom_next;

	public:
		Data()
			: atom_chunk(nullptr),
			  atom_next(nullptr) { }

		Data(Data&& o) noexcept
			: atom_chunk(o.atom_chunk.load()),
			  atom_next(o.atom_next.load()) { }

		~Data() noexcept {
			try {
				auto next = atom_next.exchange(nullptr);
				while (next) {
					auto next_next = next->atom_next.exchange(nullptr);
					delete next;
					next = next_next;
				}

				auto chunk = atom_chunk.exchange(nullptr);
				if (chunk) {
					for (auto& atom_ptr : *chunk) {
						auto ptr = atom_ptr.exchange(nullptr);
						if (ptr) {
							delete ptr;
						}
					}
					delete chunk;
				}
			} catch (...) {
				L_EXC("Unhandled exception in destructor");
			}
		}

		StashState get(std::atomic<_Tp*>** pptr_atom_ptr, size_t slot, bool spawn) {
			if (!spawn && !atom_next && !atom_chunk) {
				return StashState::StashEmpty;
			}

			auto _data = this;
			if (slot >= _Size) {
				size_t chunk_num = slot / _Size;
				slot = slot % _Size;

				for (size_t c = 0; c < chunk_num; ++c) {
					auto next = _data->atom_next.load();
					if (!next) {
						if (!spawn) {
							return StashState::StashShort;
						}
						auto tmp = std::make_unique<Data>();
						if (_data->atom_next.compare_exchange_strong(next, tmp.get())) {
							next = tmp.release();
						}  // else: unique_ptr frees the loser (exception-safe)
					}
					_data = next;
				}
			}

			auto chunk = _data->atom_chunk.load();
			if (!chunk) {
				if (!spawn) {
					return StashState::ChunkEmpty;
				}
				auto tmp = std::make_unique<Chunks>();
				if (_data->atom_chunk.compare_exchange_strong(chunk, tmp.get())) {
					chunk = tmp.release();
				}  // else: unique_ptr frees the loser (exception-safe)
			}

			auto& atom_ptr = (*chunk)[slot];

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
				auto ptr = atom_ptr.load();
				if (ptr) {
					if (ctx.op == StashContext::Operation::clean) {
						// Reclaim: once a slot's whole window is behind the safe
						// cutoff (ctx.end_key), the single walker is provably past it
						// and no producer can write the past, so the whole subtree can
						// be dropped in one shot with no further checks or locks.
						// new_first_valid_key is this slot's upper bound; only drop
						// slots entirely behind the cutoff, never the straddling one.
						if (new_first_valid_key <= ctx.end_key) {
							ptr = atom_ptr.exchange(nullptr);
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
			auto first_valid_key = ctx.atom_first_valid_key.load();
			while (new_first_valid_key > first_valid_key && !ctx.atom_first_valid_key.compare_exchange_weak(first_valid_key, new_first_valid_key));
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
		auto ptr = atom_ptr.load();
		if (!ptr) {
			auto tmp = std::make_unique<_Tp>();
			if (atom_ptr.compare_exchange_strong(ptr, tmp.get())) {
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
		auto horizon = get_end_base_key(ctx.atom_first_valid_key.load());
		if (horizon > ctx.horizon_margin) {
			horizon -= ctx.horizon_margin;
		}
		if (key >= horizon) {
			throw std::out_of_range("stash overlow");
		}

		put(ctx, key, std::forward<Args>(args)...);

		auto first_valid_key = ctx.atom_first_valid_key.load();
		auto last_valid_key = ctx.atom_last_valid_key.load();
		L_STASH("StashSlots::" + LIGHT_PURPLE + "ADD" + CLEAR_COLOR + " - _Mod:{}, key:{}, atom_first_valid_key:{}, atom_last_valid_key:{}", _Mod, key, first_valid_key, last_valid_key);
		while (key < first_valid_key && !ctx.atom_first_valid_key.compare_exchange_weak(first_valid_key, key));
		while (key > last_valid_key && !ctx.atom_last_valid_key.compare_exchange_weak(last_valid_key, key));
	}
};


template <typename _Tp, size_t _Size>
class StashValues : public Stash<_Tp, _Size> {
	using Stash_T = Stash<_Tp, _Size>;

	size_t walk_cur;
	size_t clean_cur;
	std::atomic_size_t atom_end;     // producer reserve frontier (next slot to claim)
	std::atomic_size_t atom_ready;   // contiguous written frontier (<= atom_end);
	                                 // slots [0, atom_ready) are guaranteed written

public:
	StashValues(StashValues&& o) noexcept
		: Stash_T::Stash(std::move(o)),
		  walk_cur(std::move(o.walk_cur)),
		  clean_cur(std::move(o.clean_cur)),
		  atom_end(o.atom_end.load()),
		  atom_ready(o.atom_ready.load()) { }

	StashValues()
		: walk_cur(0),
		  clean_cur(0),
		  atom_end(0),
		  atom_ready(0) { }

	template <typename T>
	bool next(StashContext& ctx, T* value_ptr, unsigned long long) {
		auto cur = (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur;

		auto loop = cur < ((ctx.op == StashContext::Operation::clean) ? walk_cur : atom_ready.load());

		while (loop) {
			auto new_cur = cur + 1;

			L_DEBUG_HOOK("StashValues::LOOP", "StashValues::" + LIGHT_SKY_BLUE + "LOOP" + CLEAR_COLOR + " - {}cur:{}, cur:{}, atom_end:{}, op:{}", ctx._col(), cur, (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur, atom_end.load(), ctx._op());

			std::atomic<_Tp*>* ptr_atom_ptr = nullptr;
			switch (Stash_T::get(&ptr_atom_ptr, cur, false)) {
				case StashState::Ok:
					break;
				case StashState::ChunkEmpty:
					L_STASH("StashValues::" + SADDLE_BROWN + "EMPTY" + CLEAR_COLOR + " - {}cur:{}, cur:{}, atom_end:{}, op:{}", ctx._col(), cur, (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur, atom_end.load(), ctx._op());
					break;
				case StashState::StashShort:
				case StashState::StashEmpty:
					L_STASH("StashValues::" + SADDLE_BROWN + "BREAK" + CLEAR_COLOR + " - {}cur:{}, cur:{}, atom_end:{}, op:{}", ctx._col(), cur, (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur, atom_end.load(), ctx._op());
					return false;
			}

			loop = new_cur < ((ctx.op == StashContext::Operation::clean) ? walk_cur : atom_ready.load());

			if (loop) {
				switch (ctx.op) {
					case StashContext::Operation::peep:
						cur = new_cur;
						break;
					case StashContext::Operation::walk:
						cur = walk_cur = new_cur;
						break;
					case StashContext::Operation::clean:
						cur = clean_cur = new_cur;
						break;
				}
			}

			if (ptr_atom_ptr) {
				auto& atom_ptr = *ptr_atom_ptr;
				auto ptr = atom_ptr.load();
				if (ptr) {
					bool returning = false;
					if (ctx.op != StashContext::Operation::clean) {
						if (*ptr && **ptr) {
							L_STASH("StashValues::" + LIGHT_GREEN + "FOUND" + CLEAR_COLOR + " - {}cur:{}, cur:{}, atom_end:{}, op:{}", ctx._col(), cur, (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur, atom_end.load(), ctx._op());
							if (value_ptr) {
								*value_ptr = *ptr;
							}
							returning = true;
						}
					}
					if (ctx.op != StashContext::Operation::peep) {
						ptr = atom_ptr.exchange(nullptr);
						if (ptr) {
							L_STASH("StashValues::" + LIGHT_RED + "CLEAR" + CLEAR_COLOR + " - {}cur:{}, cur:{}, atom_end:{}, op:{}", ctx._col(), cur, (ctx.op == StashContext::Operation::clean) ? clean_cur : walk_cur, atom_end.load(), ctx._op());
							delete ptr;
						}
					}
					if (returning) {
						return true;
					}
				}
			}
		}

		return false;
	}

	template<typename... Args>
	void put([[maybe_unused]] StashContext& ctx, unsigned long long, Args&&... args) {
		auto slot = atom_end++;
		L_STASH("StashValues::" + LIGHT_PURPLE + "PUT" + CLEAR_COLOR + " - {}slot:{}, atom_end:{}, op:{}", ctx._col(), slot, atom_end.load(), ctx._op());

		std::atomic<_Tp*>* ptr_atom_ptr;
		Stash_T::get(&ptr_atom_ptr, slot, true);

		auto& atom_ptr = *ptr_atom_ptr;
		auto ptr = atom_ptr.load();
		if (!ptr) {
			auto tmp = std::make_unique<_Tp>(std::forward<Args>(args)...);
			if (atom_ptr.compare_exchange_strong(ptr, tmp.get())) {
				ptr = tmp.release();
			}  // else: unique_ptr frees the loser (exception-safe)
		}

		// COMMIT: advance atom_ready over the contiguous *written* prefix, but only
		// up to the reserve frontier observed *now*. Snapshotting atom_end bounds
		// the loop -- re-reading it each iteration lets one producer chase an
		// ever-growing reserve count under load and never terminate (a livelock).
		// Whoever fills the slot that closes a gap drags the frontier past every
		// slot already written behind it; a hole stops the advance until its own
		// producer fills it (out-of-order fills handled exactly). Slots reserved
		// after this snapshot are advanced by their own producers' COMMIT passes.
		auto end = atom_end.load(std::memory_order_acquire);
		auto r = atom_ready.load(std::memory_order_acquire);
		while (r < end) {
			std::atomic<_Tp*>* rp = nullptr;
			if (Stash_T::get(&rp, r, false) != StashState::Ok ||
			    !rp || !rp->load(std::memory_order_acquire)) {
				break;   // slot r not written yet (a hole); its producer will advance it
			}
			if (atom_ready.compare_exchange_weak(r, r + 1,
			        std::memory_order_acq_rel, std::memory_order_acquire)) {
				++r;
			}  // else: r reloaded to the current frontier; retry (still < end)
		}
	}
};


#ifdef L_STASH_DEFINED
#undef L_STASH_DEFINED
#undef L_STASH
#endif
