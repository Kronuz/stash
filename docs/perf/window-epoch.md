# Windowed announce board with far-insert epochs

Branch `perf/window-epoch` from main `a41b2d0`. Prototype, not for merging.

## Target

The producer side of the in-flight announce ("Inserts in flight" in ARCHITECTURE.md): every
`add` claims one of 64 board slots, and more than 64 inserts in flight at once share the
overflow counter, which freezes `first_valid`. The query class is insert-heavy wheels whose
keys sit ahead of the walk: deferred logging and debouncers (keys 100 ms to 3 s out), measured by
`bench_bursty` (256 and 1,024 mostly idle producers) and `bench_structures` (spread keys).

## Hypothesis

Only an insert whose key the walk can reach before the next window move needs its key on the
board. The walk publishes a window limit `W`, a few finest slots past the insert floor, and moves
it forward in chunks; each move opens an epoch. An insert with key `>= W` skips the key slot and
instead increments a striped count for the epoch of the `W` it read, on its remembered board line,
then decrements it when its value is in place. While the previous epoch's count is nonzero the
walk keeps firing but holds `first_valid` at that epoch's `W`. Expected effect: overflow on
`bench_bursty` drops to near zero (most keys are far), with a small producer cost on x86 (the far
path is two locked RMWs, the board path one CAS and one plain store).

## Procedure

1. Rebuild an exhaustive interleaving model of add, walk and clean (`.scratch/model/`), check
   main and the variant for no loss, no premature `first_valid` and no use-after-free by `clean`,
   including a producer stalled across several window moves, and a mutant without the epoch count
   that must lose.
2. Interleaved A/B against main, 5 rounds, under `quiet run` holds: `bench_structures` stash rows
   (spread and convergent, 1/2/4/8/14 threads, throughput and p99), `bench_bursty` at 256 and
   1,024 threads (adds/s, p50/p99/p999, overflows, far inserts, lost), the starved stress at lead 0
   (40 runs each), TSan and ASan on `test/concurrent.cc` and `test/test.cc`.
3. Overflow handling in measure-first order (owner's direction): report the overflow rate; only
   if it still overflows, add an adaptive window shrink; only if an overflow cleanup delay is
   measured to matter, replace the freeze with an overflow floor.

## Keep or remove

Keep (recommend) if overflow drops to near zero at 1,024 bursty threads, no correctness gate
fails, and producer throughput and p99 lose no more than the run-to-run spread on spread and
convergent keys at every thread count. A measurable throughput or p99 loss is stated next to the
overflow win and weighed; any loss, model violation or sanitizer report removes it.

## Results

Pending.
