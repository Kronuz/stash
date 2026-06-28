# Results

A representative run of the [benchmarks](README.md). Your numbers will differ;
the *shape* is what matters.

## Machine & method

- **Machine** — Apple M4 Pro, 10 performance + 4 efficiency cores, 14 threads.
- **Toolchain** — AppleClang 17, C++20, `-O3 -DNDEBUG`.
- **Method** — N producer threads insert a fixed total of `shared_ptr` tasks as
  fast as they can; one consumer thread drains concurrently the whole time.
  Throughput is total inserts over producer wall-clock; latency is measured per
  insert and reported as percentiles. Best of three runs.
- **`bench_structures`** uses 2,000,000 inserts; **`bench_scheduler`** uses
  1,000,000 (Asio allocates a full `steady_timer` per arm, and two million live
  timers is a lot of memory).
- These numbers are on the current `stash.h` (post strand-fix, walk-based clean,
  and the consumer cursor). The strand fix is what makes the convergent column
  below slower than a plain queue — see the hand-off table.

## The one caveat that matters

The harness lets the pending set grow to millions of entries. That inflates the
heap's `O(log n)` at low thread counts far more than a real scheduler, which
holds dozens to thousands of pending items, ever would. **So the robust signal
here is the tail latency (`p99`) and the *shape* of the scaling, not the
single-threaded throughput.** The heap's real problem is not its `log n`; it is
that every producer waits behind one lock, which shows up as the `p99` column.
`asio::steady_timer` is the same story in a different shape: its throughput stops
scaling under contention while its tail latency blows up roughly 40×.

## Data structure (`bench_structures`, 2,000,000 inserts)

Throughput in **million inserts/sec** (higher is better), with `p99` insert
latency underneath.

### Scheduler role — spread keys `now + uniform(0, 100ms)` (the real pattern)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 1.99 | 2.52 | 3.44 | 4.86 | 5.85 |
| stash p99 | 1.3µs | 1.9µs | 2.8µs | 3.8µs | 4.8µs |
| heap (mutex) | 2.03 | 1.32 | 1.64 | 4.39 | 6.11 |
| heap p99 | 13µs | 33µs | 46µs | 36µs | 45µs |
| multimap (mutex) | 1.64 | 0.93 | 0.74 | 1.37 | 1.62 |
| multimap p99 | 8µs | 31µs | 50µs | 78µs | 85µs |

stash and the heap converge on throughput once a dozen threads contend, but the
heap gets there with **10–16× the tail latency**, because its producers queue
behind one lock. multimap is strictly worse.

### Hand-off / stress — convergent keys `now()` (Xapiand runs these inline, not via stash)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.71 | 2.84 | 2.91 | 3.03 | 2.59 |
| stash p99 | 0.7µs | 1.3µs | 2.3µs | 4.2µs | 8.7µs |
| deque (mutex) | 3.75 | 3.24 | 3.90 | 5.77 | 8.11 |
| vyukov (lock-free MPSC) | 7.02 | 9.80 | 10.65 | 14.75 | 12.77 |
| sharded (per-producer) | 3.50 | 12.21 | 23.91 | 34.26 | 45.36 |
| heap (mutex) | 3.83 | 2.68 | 3.15 | 5.30 | 6.69 |

This is the one pattern stash is built to avoid, and it shows: when every key is
`now()` every producer piles onto the same leaf, and the per-leaf COMMIT loop
that makes reclamation lossless serializes them. stash is the **slowest** thing
in this table — a **per-producer sharded queue** wins by a wide margin (45 vs
stash's 2.6 at 14 threads) and scales almost linearly, a single lock-free queue
(Vyukov) saturates at ~13, and even a plain mutex+deque (8) beats it. A queue is
the right tool for hand-off; stash is the wrong one — but a queue cannot answer
"what is due now," which is the job stash exists for, and which never produces
this access pattern.

## Scheduler shoot-out (`bench_scheduler`, 1,000,000 arms)

Throughput in **million arms/sec**, `p99` arm latency underneath. Many threads
arm timed tasks; one consumer fires them.

### Spread keys `now + uniform(0, 100ms)` (the real pattern)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.03 | 2.55 | 3.55 | 5.06 | 6.13 |
| stash p99 | 1.3µs | 1.9µs | 2.7µs | 3.8µs | 4.8µs |
| heap (mutex) | 2.07 | 1.34 | 1.94 | 3.86 | 5.55 |
| heap p99 | 12µs | 30µs | 37µs | 36µs | 45µs |
| `asio::steady_timer` | 3.68 | 2.73 | 2.33 | 3.39 | 4.45 |
| asio p99 | 1.5µs | 9.3µs | 22µs | 36µs | 59µs |

### Convergent keys `now()`

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.72 | 2.85 | 2.93 | 3.01 | 2.61 |
| stash p99 | 0.7µs | 1.3µs | 2.3µs | 4.2µs | 8.7µs |
| heap (mutex) | 2.70 | 2.80 | 3.14 | 4.61 | 6.03 |
| heap p99 | 7.5µs | 13µs | 21µs | 32µs | 39µs |
| `asio::steady_timer` | 6.73 | 4.09 | 2.93 | 3.99 | 5.50 |
| asio p99 | 1µs | 8.5µs | 20µs | 34µs | 54µs |

`asio::steady_timer` is the **fastest of the three with one or two threads**, and
then it stops scaling: under contention its throughput sags (it never beats its
own one-thread number on the convergent load) while its tail latency climbs to
~60µs, roughly 40× its single-thread tail, because every `async_wait` serializes
on the `io_context`'s lock. The stash-backed scheduler does the opposite — it
keeps climbing to 6.1 and holds a ~5µs tail. That tail gap, not the average, is
why it wins the load Xapiand actually puts on it. Asio remains the better choice
when the timer load is light or you want its composition and cancellation
machinery.

## Notes

- One `steady_timer` per scheduled task is the honest way to ask Asio "run this
  at time T" — Asio has no fire-at-T primitive without a timer object. Pooling
  timers means rebuilding a scheduler on top of Asio.
- The Vyukov queue is the canonical intrusive lock-free MPSC. The sharded queue
  is a per-producer `deque` behind a per-shard lock, the shape moodycamel's
  `ConcurrentQueue` uses internally.
