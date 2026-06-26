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

## The one caveat that matters

The harness lets the pending set grow to millions of entries. That inflates the
heap's `O(log n)` at low thread counts far more than a real scheduler, which
holds dozens to thousands of pending items, ever would. **So the robust signal
here is the tail latency (`p99`) and the *shape* of the scaling, not the
single-threaded throughput.** The heap's real problem is not its `log n`; it is
that every producer waits behind one lock, which shows up as the `p99` column and
as the way `asio::steady_timer`'s throughput goes *backwards* as threads are
added.

## Data structure (`bench_structures`, 2,000,000 inserts)

Throughput in **million inserts/sec** (higher is better), with `p99` insert
latency underneath.

### Scheduler role — spread keys `now + uniform(0, 100ms)` (the real pattern)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.09 | 2.54 | 3.63 | 4.95 | 5.85 |
| stash p99 | 1.2µs | 1.8µs | 2.4µs | 3.5µs | 4.3µs |
| heap (mutex) | 0.64 | 1.10 | 1.51 | 4.15 | 5.91 |
| heap p99 | 21µs | 33µs | 42µs | 36µs | 55µs |
| multimap (mutex) | 1.50 | 0.88 | 0.85 | 1.48 | 1.66 |
| multimap p99 | 5µs | 25µs | 43µs | 77µs | 140µs |

stash and the heap converge on throughput once a dozen threads contend, but the
heap gets there with **10–18× the tail latency**, because its producers queue
behind one lock. multimap is strictly worse.

### Hand-off / stress — convergent keys `now()` (Xapiand runs these inline, not via stash)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 3.21 | 4.42 | 5.29 | 6.81 | 7.45 |
| stash p99 | 0.5µs | 0.7µs | 1.2µs | 1.8µs | 2.8µs |
| deque (mutex) | 4.06 | 3.45 | 4.23 | 6.19 | 7.99 |
| vyukov (lock-free MPSC) | 7.86 | 10.28 | 12.06 | 14.96 | 12.83 |
| sharded (per-producer) | 3.58 | 11.76 | 21.55 | 31.41 | 46.93 |
| heap (mutex) | 0.42 | 1.57 | 3.70 | 5.25 | 6.70 |

For pure many-to-one hand-off with no ordering, a **per-producer sharded queue**
wins by a wide margin (47 vs stash's 7.5 at 14 threads) and scales almost
linearly — it has no shared hot point. A single lock-free queue (Vyukov) funnels
through one atomic exchange and saturates at ~13. stash sits at the ceiling of
its two shared atomics (the leaf cursor and the valid-key bounds). This is *why*
a sharded queue is the right tool for hand-off, and why stash is the wrong tool
for it — but a queue cannot answer "what is due now," which is the job stash
exists for.

## Scheduler shoot-out (`bench_scheduler`, 1,000,000 arms)

Throughput in **million arms/sec**, `p99` arm latency underneath. Many threads
arm timed tasks; one consumer fires them.

### Spread keys `now + uniform(0, 100ms)` (the real pattern)

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 2.17 | 2.69 | 3.76 | 5.22 | 6.09 |
| stash p99 | 1.2µs | 1.8µs | 2.4µs | 3.6µs | 4.5µs |
| heap (mutex) | 2.70 | 1.46 | 1.82 | 4.69 | 5.07 |
| heap p99 | 7µs | 26µs | 33µs | 32µs | 40µs |
| `asio::steady_timer` | 2.74 | 3.60 | 3.10 | 1.51 | 1.51 |
| asio p99 | 0.6µs | 0.8µs | 7µs | 30µs | 59µs |

### Convergent keys `now()`

| threads | 1 | 2 | 4 | 8 | 14 |
| --- | --- | --- | --- | --- | --- |
| **stash** | 3.05 | 4.57 | 5.38 | 6.87 | 7.72 |
| stash p99 | 0.5µs | 0.7µs | 1.2µs | 1.8µs | 2.7µs |
| heap (mutex) | 4.40 | 2.84 | 3.42 | 5.16 | 6.10 |
| heap p99 | 3µs | 11µs | 19µs | 29µs | 35µs |
| `asio::steady_timer` | 4.79 | 4.85 | 3.19 | 1.59 | 1.62 |
| asio p99 | 0.3µs | 0.5µs | 7µs | 25µs | 60µs |

`asio::steady_timer` is the **fastest of the three with one or two threads**, and
the slowest by a wide margin once a handful contend: its throughput drops *below*
its single-threaded number, because every `async_wait` serializes on the
`io_context`'s lock. That negative scaling, not the average, is the reason the
stash-backed scheduler wins for the load Xapiand actually puts on it. Asio
remains the better choice when the timer load is light or you want its
composition and cancellation machinery.

## Notes

- One `steady_timer` per scheduled task is the honest way to ask Asio "run this
  at time T" — Asio has no fire-at-T primitive without a timer object. Pooling
  timers means rebuilding a scheduler on top of Asio.
- The Vyukov queue is the canonical intrusive lock-free MPSC. The sharded queue
  is a per-producer `deque` behind a per-shard lock, the shape moodycamel's
  `ConcurrentQueue` uses internally.
