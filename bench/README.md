# stash benchmarks

The harness behind the numbers in [*The Sparse Wheel*](https://kronuz.github.io/blog/the-sparse-wheel/).
It builds the real `stash.h` wheel with Xapiand's exact four-level configuration
and measures the producer-side insert cost against the realistic alternatives,
under the two access patterns that actually occur:

- **spread** — `key = now + uniform(0, W)`, the real deferred-log / debounce / timer pattern.
- **convergent** — `key = now`, the pathological all-at-once pattern (Xapiand routes
  its immediate logs inline, so this never actually hits the wheel; it is a stress test).

One consumer thread drains concurrently in every run, so the numbers include
realistic add/walk contention on the shared atomics, not an insert microbench in
isolation.

## Build & run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/bench_structures           # stash vs mutex-deque / Vyukov MPSC / sharded queue / heap
./build/bench_scheduler            # stash vs asio::steady_timer vs mutex+heap

./build/bench_structures 4000000   # optional: override total op count
```

`bench_structures` has no dependencies beyond `stash.h`. `bench_scheduler`
additionally fetches standalone [Asio](https://github.com/chriskohlhoff/asio)
(header-only) to compare against `asio::steady_timer`.

## What each one measures

| binary | structures | role |
| --- | --- | --- |
| `bench_structures` | stash, `mutex`+heap, `mutex`+multimap, `mutex`+deque, lock-free Vyukov MPSC, per-producer sharded queue | data-structure insert cost, scheduler role vs hand-off role |
| `bench_scheduler` | stash, `mutex`+heap, `asio::steady_timer` | the scheduler shoot-out: arming timed tasks from many threads |

See [RESULTS.md](RESULTS.md) for a representative run, the methodology, and the
one caveat that matters when reading the tables.
