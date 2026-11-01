# Results

A recorded run of the [benchmarks](README.md). Your numbers will differ; the *shape* is
what matters.

## Machine & method

- **Machine**: Apple M4 Pro, 10 performance + 4 efficiency cores, 14 threads, macOS 26.
  The host also runs other work, and the p99 columns move by up to 2x between runs.
- **Toolchain**: Apple clang 17.0.0, C++20, `-O3 -DNDEBUG` (CMake `Release`), Asio 1.30.2
  (fetched by `bench/CMakeLists.txt`), `stash.h` as of commit `78a18f3`.
- **Method**: N producer threads insert/arm tasks as fast as they can; one
  consumer thread drains concurrently the whole time. Throughput is total inserts
  over producer wall-clock; latency is per insert, reported as percentiles. Each
  invocation is best of three (`bench_steady`: best of two).
- **`bench_structures`** uses 2,000,000 inserts; **`bench_scheduler`** uses
  1,000,000 (Asio allocates a full `steady_timer` per arm); **`bench_steady`**
  holds a bounded pending depth and measures per-arm latency at that depth. The
  `bench_steady` tables are the median of four invocations; the other tables are
  one invocation. The README's Performance section also has the Intel Core
  i9-9980HK.

## The headline: tail latency at realistic depth

This is the number that matters and the honest one. `bench_structures` and
`bench_scheduler` let the pending set grow to millions, which inflates a heap's
`O(log n)` far past what a real scheduler (dozens to thousands pending) ever sees,
so read those for *tail shape*, not single-thread throughput. `bench_steady` holds
a realistic depth, which is where a scheduler actually lives.

**`bench_steady`, 14 threads arming, one draining. Insert latency, ns.**

| depth | stash p99 / p999 | heap p99 / p999 | asio p99 / p999 |
| --- | --- | --- | --- |
| 64 | **2,125 / 3,833** | 913,541 / 4,040,854 | 591,333 / 3,791,312 |
| 1,024 | **1,375 / 2,896** | 119,500 / 482,000 | 143,916 / 2,544,333 |
| 16,384 | **1,208 / 2,083** | 62,771 / 226,167 | 161,354 / 2,053,583 |

stash holds a low-single-digit-microsecond p99 across depth; the locked heap and
Asio spike to tens or hundreds of microseconds, and to **hundreds of microseconds
or more** at shallow depth under 14 writers. The cause is structural, not a harness artifact: a
lock makes producers queue, and under contention one of them eventually waits a
scheduler quantum. stash's producers never block each other, so a slow insert
stays a slow insert instead of becoming a stall. A lock-free queue keeps a flat
tail too (p99 in the tens of ns) but has no notion of *when*; that is the trade
this whole structure exists to make.

Per-thread detail (p50 / p99 / p999, ns):

| | depth 64 | depth 1,024 | depth 16,384 |
| --- | --- | --- | --- |
| stash, 1 thread | 84 / 500 / 771 | 83 / 416 / 625 | 42 / 292 / 708 |
| stash, 4 threads | 292 / 1,000 / 1,416 | 271 / 854 / 1,125 | 250 / 834 / 1,125 |
| stash, 14 threads | 625 / 2,125 / 3,833 | 458 / 1,375 / 2,896 | 417 / 1,208 / 2,083 |
| heap, 14 threads | 10,958 / 913,541 / 4,040,854 | 83 / 119,500 / 482,000 | 42 / 62,771 / 226,167 |
| asio, 14 threads | 13,249 / 591,333 / 3,791,312 | 5,063 / 143,916 / 2,544,333 | 1,416 / 161,354 / 2,053,583 |

(The heap's low p50 with a huge p99 is the lock exactly: most acquisitions are
cheap, a few wait a quantum. That spread is the tail story in one row.)

## Scheduler role — spread keys `now + uniform(0, 100ms)` (the real pattern)

Throughput in **million inserts/sec**, `p99` insert latency underneath.

### `bench_structures` (2,000,000 inserts)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 10.00 | 13.22 | 17.90 | 19.13 | 22.85 |
| stash p99 | 0.13µs | 0.50µs | 0.58µs | 0.71µs | 0.92µs |
| heap (mutex) | 1.60 | 1.46 | 1.59 | 4.48 | 5.79 |
| heap p99 | 10µs | 30µs | 48µs | 38µs | 52µs |
| multimap (mutex) | 1.85 | 0.97 | 0.76 | 1.39 | 1.63 |
| multimap p99 | 7.2µs | 32µs | 49µs | 72µs | 111µs |

### `bench_scheduler` (1,000,000 arms, vs `asio::steady_timer`)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 9.58 | 11.16 | 18.38 | 19.70 | 22.54 |
| stash p99 | 0.21µs | 0.33µs | 0.46µs | 0.67µs | 0.92µs |
| heap (mutex) | 1.72 | 1.51 | 1.75 | 3.61 | 5.78 |
| heap p99 | 10.5µs | 27µs | 36µs | 34µs | 52µs |
| `asio::steady_timer` | 4.37 | 3.00 | 2.61 | 3.42 | 4.38 |
| asio p99 | 1.4µs | 8.4µs | 20µs | 35µs | 66µs |

stash is the fastest of the three at every thread count, and at 14 threads it takes
**4x the heap's throughput at 1/57 of its p99**, because the heap's producers queue
behind one lock. Asio's throughput stops growing with threads and its p99 climbs to
66µs.

## Hand-off / stress — convergent keys `now()`

The case the wheel is built to avoid: every producer piles onto one leaf. Xapiand
runs this load inline, never through stash. It is the fairest place to be
skeptical, so it is here in full.

### `bench_structures`, convergent (2,000,000 inserts)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 4.94 | 6.56 | 7.16 | 8.53 | 7.87 |
| stash p99 | 0.63µs | 0.79µs | 1.0µs | 1.5µs | 2.8µs |
| deque (mutex) | 3.67 | 3.44 | 4.19 | 6.06 | 7.68 |
| vyukov (lock-free MPSC) | 6.56 | 10.58 | 13.46 | 14.78 | 12.77 |
| sharded (per-producer) | 3.73 | 12.71 | 25.63 | 32.36 | 42.56 |
| heap (mutex) | 2.65 | 2.76 | 2.75 | 5.38 | 6.71 |

At 14 threads stash takes 7.9 M/s, which beats the heap (6.7) and ties a locked deque
(7.7), with the tightest tail of the locked options (2.8µs p99). A per-producer
**sharded queue** wins hand-off by a wide margin (42.6) and scales near-linearly,
and a single lock-free queue (Vyukov) saturates at about 13 to 15: a queue is the right
tool for pure hand-off, and stash is competitive here without being built for it. A queue
cannot answer "what is due now," which is the job stash exists for.

### `bench_scheduler`, convergent (1,000,000 arms)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 4.76 | 5.84 | 8.11 | 8.46 | 7.86 |
| stash p99 | 0.58µs | 0.83µs | 1.0µs | 1.5µs | 2.7µs |
| heap (mutex) | 2.77 | 3.10 | 3.75 | 4.75 | 6.15 |
| heap p99 | 5.9µs | 11µs | 18µs | 34µs | 47µs |
| `asio::steady_timer` | 7.02 | 4.46 | 3.79 | 4.15 | 5.50 |
| asio p99 | 1.2µs | 7.3µs | 17µs | 33µs | 58µs |

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
