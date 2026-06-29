# Results

A representative run of the [benchmarks](README.md). Your numbers will differ;
the *shape* is what matters.

## Machine & method

- **Machine** — Apple M4 Pro, 10 performance + 4 efficiency cores, 14 threads.
- **Toolchain** — AppleClang 17, C++20, `-O3 -DNDEBUG`.
- **Method** — N producer threads insert/arm tasks as fast as they can; one
  consumer thread drains concurrently the whole time. Throughput is total inserts
  over producer wall-clock; latency is per insert, reported as percentiles. Best
  of three (`bench_steady`: best of two).
- **`bench_structures`** uses 2,000,000 inserts; **`bench_scheduler`** uses
  1,000,000 (Asio allocates a full `steady_timer` per arm); **`bench_steady`**
  holds a bounded pending depth and measures per-arm latency at that depth.
- Numbers are on the current `stash.h` (sentinel frontier: the per-leaf COMMIT
  loop is gone, replaced by a three-state slot — reserved / filled / drained —
  the walker reads directly).

## The headline: tail latency at realistic depth

This is the number that matters and the honest one. `bench_structures` and
`bench_scheduler` let the pending set grow to millions, which inflates a heap's
`O(log n)` far past what a real scheduler (dozens to thousands pending) ever sees,
so read those for *tail shape*, not single-thread throughput. `bench_steady` holds
a realistic depth, which is where a scheduler actually lives.

**`bench_steady`, 14 threads arming, one draining. Insert latency, ns.**

| depth | stash p99 / p999 | heap p99 / p999 | asio p99 / p999 |
| --- | --- | --- | --- |
| 64 | **1,583 / 12,667** | 1,213,708 / 4,904,209 | 1,248,959 / 8,185,625 |
| 1,024 | **1,375 / 15,709** | 151,166 / 757,292 | 188,209 / 953,459 |
| 16,384 | **1,833 / 27,958** | 65,209 / 253,167 | 182,041 / 1,477,833 |

stash holds a low-single-digit-microsecond p99 across depth; the locked heap and
Asio spike to tens or hundreds of microseconds, and to **over a millisecond** at
shallow depth under 14 writers. The cause is structural, not a harness artifact: a
lock makes producers queue, and under contention one of them eventually waits a
scheduler quantum. stash's producers never block each other, so a slow insert
stays a slow insert instead of becoming a stall. A lock-free queue keeps a flat
tail too (p99 in the tens of ns) but has no notion of *when*; that is the trade
this whole structure exists to make.

Per-thread detail (p50 / p99 / p999, ns):

| | depth 64 | depth 1,024 | depth 16,384 |
| --- | --- | --- | --- |
| stash, 1 thread | 84 / 375 / 1,625 | 167 / 417 / 625 | 250 / 583 / 1,292 |
| stash, 4 threads | 166 / 541 / 916 | 250 / 666 / 959 | 500 / 1,000 / 1,500 |
| stash, 14 threads | 458 / 1,583 / 12,667 | 459 / 1,375 / 15,709 | 667 / 1,833 / 27,958 |
| heap, 14 threads | 12,125 / 1,213,708 / 4,904,209 | 83 / 151,166 / 757,292 | 42 / 65,209 / 253,167 |
| asio, 14 threads | 8,375 / 1,248,959 / 8,185,625 | 1,125 / 188,209 / 953,459 | 416 / 182,041 / 1,477,833 |

(The heap's low p50 with a huge p99 is the lock exactly: most acquisitions are
cheap, a few wait a quantum. That spread is the tail story in one row.)

## Scheduler role — spread keys `now + uniform(0, 100ms)` (the real pattern)

Throughput in **million inserts/sec**, `p99` insert latency underneath.

### `bench_structures` (2,000,000 inserts)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 1.90 | 2.52 | 3.57 | 4.37 | 5.02 |
| stash p99 | 1.3µs | 1.9µs | 2.5µs | 3.5µs | 3.9µs |
| heap (mutex) | 1.41 | 1.24 | 1.84 | 3.24 | 4.63 |
| heap p99 | 9.6µs | 40µs | 57µs | 52µs | 62µs |
| multimap (mutex) | 1.30 | 0.97 | 0.75 | 1.08 | 1.23 |
| multimap p99 | 7µs | 48µs | 88µs | 109µs | 164µs |

### `bench_scheduler` (1,000,000 arms, vs `asio::steady_timer`)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 1.96 | 2.60 | 3.70 | 4.60 | 5.54 |
| stash p99 | 1.3µs | 1.9µs | 2.5µs | 3.5µs | 4.3µs |
| heap (mutex) | 1.94 | 1.42 | 2.21 | 3.82 | 5.65 |
| heap p99 | 0.6µs | 46µs | 47µs | 46µs | 49µs |
| `asio::steady_timer` | 3.16 | 2.89 | 2.29 | 2.52 | 3.82 |
| asio p99 | 2.7µs | 16µs | 34µs | 51µs | 67µs |

stash and the heap converge on throughput once a dozen threads contend, but the
heap gets there with **10–15× the tail latency**, because its producers queue
behind one lock. Asio is fastest at one or two threads, then stops scaling and its
tail climbs past 60µs.

## Hand-off / stress — convergent keys `now()`

The case the wheel is built to avoid: every producer piles onto one leaf. Xapiand
runs this load inline, never through stash. It is the fairest place to be
skeptical, so it is here in full.

### `bench_structures`, convergent (2,000,000 inserts)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.70 | 3.92 | 6.21 | 6.23 | 6.78 |
| stash p99 | 0.7µs | 1.0µs | 1.1µs | 1.9µs | 2.4µs |
| deque (mutex) | 4.53 | 4.86 | 4.24 | 5.00 | 7.67 |
| vyukov (lock-free MPSC) | 6.74 | 10.51 | 16.53 | 12.67 | 14.31 |
| sharded (per-producer) | 4.75 | 10.66 | 18.33 | 34.43 | 47.01 |
| heap (mutex) | 3.39 | 3.07 | 2.87 | 4.04 | 5.92 |

stash is no longer the slowest thing here. Removing the COMMIT loop took it from
~2.6 M/s (the old worst-in-table number, behind even a mutex+deque) to **6.78**,
which beats the heap and ties a locked deque, with the tightest tail of the locked
options (2.4µs). A per-producer **sharded queue** still wins hand-off by a wide
margin (47 vs 6.8) and scales near-linearly, and a single lock-free queue (Vyukov)
saturates at ~14 — a queue is the right tool for pure hand-off, and stash being
*competitive* here just means its one ugly facet is no longer ugly. A queue still
cannot answer "what is due now," which is the job stash exists for.

### `bench_scheduler`, convergent (1,000,000 arms)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.60 | 3.78 | 6.27 | 6.19 | 6.98 |
| stash p99 | 0.7µs | 1.0µs | 1.1µs | 2.0µs | 2.3µs |
| heap (mutex) | 3.80 | 2.38 | 3.13 | 4.61 | 6.68 |
| heap p99 | 1.7µs | 28µs | 37µs | 40µs | 46µs |
| `asio::steady_timer` | 4.92 | 3.94 | 3.07 | 3.42 | 5.21 |
| asio p99 | 0.9µs | 13µs | 31µs | 43µs | 58µs |

## Notes

- The bounded-depth panel (`bench_steady`) is the one to trust; the unbounded
  throughput panels inflate the heap's `log n` and are best read for tail *shape*.
- One `steady_timer` per scheduled task is the honest way to ask Asio "run this at
  time T"; Asio has no fire-at-T primitive without a timer object.
- The Vyukov queue is the canonical intrusive lock-free MPSC. The sharded queue is
  a per-producer `deque` behind a per-shard lock, the shape moodycamel's
  `ConcurrentQueue` uses internally.
- All of these assume the wheel's span comfortably exceeds the consumer's clean
  latency. A span sized so the wheel laps its own cleaner aliases reused slots; see
  `STRAND_FIX.md`.
