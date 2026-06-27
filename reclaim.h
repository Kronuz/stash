/*
 * reclaim.h -- safe-memory-reclamation (SMR) support for stash (Path B).
 *
 * The wheel reuses physical slots (the modulus) and frees whole subtrees as the
 * clock moves past them. Doing that safely while producers insert concurrently
 * means the consumer must *know* -- not guess with a wall-clock margin -- when no
 * producer can still be touching a slot. This header provides exactly that
 * knowledge and nothing else: it never touches the wheel's data structure.
 *
 * Mechanism: each producer publishes the key it is currently inserting (its
 * "announce") in its own slot of a shared array. The consumer reads the array to
 * learn the lowest key any producer is working on (`safe_floor`). Everything
 * strictly below `safe_floor` is provably unreachable by any producer, hence safe
 * to free -- with no margin.
 *
 * This header is standalone (depends only on the standard library) and is a no-op
 * unless a wheel is given a Domain (the default is null, i.e. pre-B behavior).
 */
#pragma once

#include <array>      // for std::array
#include <atomic>     // for std::atomic
#include <cstddef>    // for std::size_t
#include <limits>     // for std::numeric_limits

namespace stash_reclaim {

// One stable index per thread, assigned on first use from a shared counter and
// reused for the life of the thread. Because each producer writes only its own
// slot, announcing is contention-free: no two threads ever touch the same slot.
inline std::atomic<int>& thread_counter() {
	static std::atomic<int> c{0};
	return c;
}
inline int thread_index() {
	static thread_local int idx = thread_counter().fetch_add(1, std::memory_order_relaxed);
	return idx;
}

// The reclamation domain shared by all producers and the single consumer of one
// wheel. It is just the announce array; the policy that uses it lives in the wheel.
template <std::size_t MaxThreads = 256>
struct Domain {
	static constexpr unsigned long long IDLE =
		std::numeric_limits<unsigned long long>::max();

	std::array<std::atomic<unsigned long long>, MaxThreads> announce;

	Domain() {
		for (auto& a : announce) {
			a.store(IDLE, std::memory_order_relaxed);
		}
	}

	// Producer side. `enter` publishes the in-flight key; `leave` clears it.
	// Release ordering pairs with the consumer's acquire in `safe_floor`, so any
	// free the consumer decides on is ordered after this announce is visible.
	void enter(unsigned long long key) {
		int i = thread_index();
		if (i < static_cast<int>(MaxThreads)) {
			announce[i].store(key, std::memory_order_release);
		}
	}
	void leave() {
		int i = thread_index();
		if (i < static_cast<int>(MaxThreads)) {
			announce[i].store(IDLE, std::memory_order_release);
		}
	}

	// Consumer side. The lowest key any producer is currently inserting (`IDLE` if
	// none). Nothing strictly below this can be reached by a producer.
	unsigned long long safe_floor() const {
		unsigned long long m = IDLE;
		int n = thread_counter().load(std::memory_order_acquire);
		if (n > static_cast<int>(MaxThreads)) {
			n = static_cast<int>(MaxThreads);
		}
		for (int i = 0; i < n; ++i) {
			unsigned long long k = announce[i].load(std::memory_order_acquire);
			if (k < m) {
				m = k;
			}
		}
		return m;
	}
};

// RAII: publish a key as in-flight for the duration of one insert, releasing it
// even on an exception. A null domain (the default, pre-B behavior) is a no-op.
template <typename DomainT>
struct InFlight {
	DomainT* dom;
	InFlight(DomainT* d, unsigned long long key) : dom(d) {
		if (dom) dom->enter(key);
	}
	~InFlight() {
		if (dom) dom->leave();
	}
	InFlight(const InFlight&) = delete;
	InFlight& operator=(const InFlight&) = delete;
};

}  // namespace stash_reclaim
