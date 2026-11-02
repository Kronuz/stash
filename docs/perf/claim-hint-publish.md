# Claim hint: does main still cost depth-64 p99, and a rare publish

> Status: CURRENT (2026-11-02), on branch perf/claim-hint-publish; M4 Pro and Linux not measured. Workstream perf/claim-hint-publish (stash#1). Machine: Intel
> i9-9980HK, macOS, Apple clang 17, `-O3 -DNDEBUG`. The M4 Pro is not reachable from this host.
> Paths under `.scratch/` are local files (build scripts, raw outputs) that are not in the repository.

## Target

`bench_steady`'s stash rows at depth 64 (4 and 14 producers, p99): a leaf holds about 120 values in
chunks of 10, so 35% of the inserts land past `claim_hint_chunks` (8) chunks and read the hint, and
about one in ten of those publishes it with a compare-exchange on the leaf's line.

## Baseline A/B: START (2e2b75f) against main (8fa5d2a)

See "Results". Procedure: `.scratch/rounds.sh`, interleaved START, main, START again per round (the
A/A control), `quiet strict` holds of at most 10 minutes, ratio START / main so that below 1 means
main is slower; `.scratch/analyze.py` reports the median and range of the paired ratios.

## Experiments

Screens use the stash rows of `bench_steady` at depth 64 only (`.scratch/screen.sh`: 250 ms per row,
best of two, 47 to 85 rounds each of START, the variants, START again), one strict hold each. Cells
are the median of START / variant p99 at t=4 / t=14; below 1 means the variant is slower. A/A
(START against START) was 0.987 to 1.006 in every screen.

| # | variant | START / variant p99, t=4 / t=14 | verdict |
| --- | --- | --- | --- |
| 0 | main (8fa5d2a): compare-exchange before the fill, hint past 8 chunks | 0.934 to 0.955 / 0.938 to 0.946 (four screens) | the cost |
| 1 | release store after the fill, only when no hint or K nodes behind: K = 2, 4, 8, never again | 0.976 / 0.981, 0.970 to 0.980 / 0.976 to 0.986, 0.965 / 0.981, 0.978 / 0.980 | halves the cost, not parity; the rate is not the cause |
| 1b | same, by slot: every 2nd, 4th, 8th chunk's first slot | 0.970 / 0.990, 0.967 to 0.975 / 0.984 to 0.985, 0.983 / 0.986 | same |
| 2 | hint next to `atom_end`, walker fields behind it (no change in size) | main layout 0.939 / 0.937; with K=4 0.957 / 0.970; with K=2 0.946 / 0.974 | loser: no gain, K=4 worse; removed |
| 3 | threshold 12 and 16 chunks with a rare publish (16 reads the hint 0 times at depth 64) | 0.964 / 0.984, 0.973 / 0.991; 16 alone 0.976 / 0.993 | the residual 1 to 2.5% is not the hint; removed |
| 4 | stride 2 for the first 8 or 16 chunks, every chunk after | 0.976 / 0.984, 0.986 / 0.989 | `bench_bursty` p50 cost: stride 2 vs main 0.954 and 0.970 (256 and 1 024 threads), stride 4 0.902 and 0.920 (4 rounds): each extra hop is a cold line. Removed |
| 5 | **release store after the fill at each chunk's first slot; every claim past the head chunk reads the hint; no threshold** | **0.993 / 0.995 (54 rounds); final build 0.989 / 0.995 (84 rounds)** | kept |

Why 5 and not 0: main publishes before the fill, so the fill and everything behind it waits for an
ordered write on the leaf's line that every claim contends; here nothing waits for the store. That
is the likely mechanism, not a proved one: variant 1 shows the publish rate is not what costs, and
variant 3 shows the reads are not.

## Results

All numbers: Intel i9-9980HK, strict holds (exclusive on macOS), `bench_steady 250`; ratio is
START / row or main / row, so below 1 means the right-hand side is slower. The A/A column is START
against START in the same rounds.

### Before and after: stash, depth 64 (ns, medians of 84 interleaved rounds)

| row | START (2e2b75f) | main (8fa5d2a) | candidate | START / main | START / candidate | A/A |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| t=4 p50 | 345 | 340 | 331 | 1.012 | 1.041 | 0.997 |
| t=4 p99 | 748 | 781 | 763 | 0.951 | 0.989 | 0.993 |
| t=14 p50 | 528 | 518 | 500 | 1.017 | 1.050 | 0.996 |
| t=14 p99 | 1 227 | 1 304 | 1 238 | 0.942 | 0.995 | 0.999 |
| t=1 p99 | 484 | 478 | 470 | 1.015 | 1.033 | 1.007 |

Main loses the depth-64 p99 in 58 of 84 rounds (t=4) and 81 of 84 (t=14); the candidate in 46 and
47 of 84, as START against itself does (44 and 42). So main cost 5% and 6%; the candidate is at
START. The candidate's p50 is 3% to 5% faster than START's.

### The other three benches and the chain-walk rows (4 to 5 rounds, interleaved, with A/A)

Geomeans over the stash rows (all thread counts, throughput and p99):

| | START / candidate | main / candidate |
| --- | ---: | ---: |
| `bench_scheduler` | 2.335 | 1.077 |
| `bench_structures` | 1.757 | 1.043 |
| `bench_bursty` | 2.409 | 1.011 |
| `bench_steady` (27 rows; 13 rounds against START, 5 against main) | 1.514 (p99 at depth 1 024: 1.38, 1.98, 1.50; at 16 384: 1.93, 3.45, 2.86) | 1.034 |

Worst rows against main: `steady D=1024 stash t=4 p50` 0.944 (252 to 268 ns; A/A 0.973),
`structures B-convergent t=2` throughput 0.950 (A/A 0.956), `bursty threads=1024 p99` 0.933 (A/A
0.923); every other row is within 4% of main and the geomeans are above 1. The `bench_bursty` and
`bench_structures` throughput and p99 against START: all above 1.

### Four axes

- Time: above.
- Memory: the leaf stays 88 bytes (same fields); no allocation change.
- Disk: none.
- Throughput: `bench_bursty` adds/s 1.000 and 0.993 against main (256 and 1 024 threads),
  `bench_scheduler` and `bench_structures` throughput geomeans above.

### Magnitude

The saving is 65 ns at the depth-64 t=14 p99 (1 304 to 1 238 ns) and 18 ns at t=4, on an insert that
runs once per timer. At the project's stated scale (Xapiand arms timers from a handful of threads at
a few hundred thousand per second) this is under one core-millisecond per second: negligible in
CPU-hours or dollars. What it settles is the tail: the p99 of an arm at shallow depth is back at the
code before the hint, with the hint's chain-walk win (depth 1 024 and 16 384 p99 rows 1.4 to 3.5 times faster than START).

### Proof

- ctest 4/4; TSan: `stash_test`, `stash_concurrent` with CI's arguments (x2) and at depth 64 with 14
  producers (x3), no reports; ASan+UBSan: `stash_test`, depth 64 (x2), `stash_concurrent`; every id
  fires exactly once.
- Mutants against `test_claim_walk` and TSan: release to relaxed in the publish (TSan reports the
  read of `Data::index()` in both runs); publish the hint instead of the node (1 998 000 steps, want
  at most 1 008); publish on slot % Size == 1 instead of 0 (1 998 steps, want at most 1 008).
- `test_claim_walk` counts the hint reads (n - Size) and publishes (n / Size - 1) and bounds the
  chain steps at n / Size + 8; main's code fails it.

### Not measured

The M4 Pro and Linux (x86_64, arm64): the owner's earlier M4 holds had the store-after-fill
candidate at 0.955 / 0.964 against START for depth-64 p99 and main at 0.956 / 0.973, so no loss
there, but it was not re-run on this version. The lead measures the other platforms on the landing
version.
