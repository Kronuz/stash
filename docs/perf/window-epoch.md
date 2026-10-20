# Windowed announce board with far-insert epochs

Branch `perf/window-epoch` from main `a41b2d0`. Kept by the owner with a fixed width; for review,
not merged. The first round below measured the adaptive variant (170e1d8); "Fixed width" at the
end re-measures what the branch now carries.

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
3. Overflow handling (owner's direction, first form): an adaptive shrink, one occupancy check per
   window move (narrow to one chunk at 32 slots taken, widen again at 8 or fewer); no overflow
   floor and no eviction of board entries. Dropped afterwards (owner): the width is fixed at the
   wide width, and overflow and holds are reported only through compile-time stats hooks.

## Keep or remove

Keep (recommend) if overflow drops to near zero at 1,024 bursty threads, no correctness gate
fails, and producer throughput and p99 lose no more than the run-to-run spread on spread and
convergent keys at every thread count. A measurable throughput or p99 loss is stated next to the
overflow win and weighed; any loss, model violation or sanitizer report removes it.

## Results

All runs on macOS x86_64 (i9-9980HK, 8 cores, 16 threads), Apple clang 17 `-O3 -DNDEBUG` for the
benches, `-O2` for the stress binaries, Homebrew clang 23.1.2 for the sanitizers. Timed runs under
strict `quiet run` holds with the host otherwise idle. The timed builds are 170e1d8; 6af4922 after
them changes only the window width before the first move.

### Model

`.scratch/model/stash_model.py` rebuilds the review's exhaustive model: sequentially consistent,
one step per atomic access (each board slot and count read, claim, floor and `W` read, reserve,
`last_valid` raise, fill, release, each walk visit and `ret_next` publish, and `clean` freeing
groups wholly below `first_valid`), 2 producers, both consumer styles (`sched`: `begin_key =
first_valid` on every call; `cont`: once per walk). It checks no use-after-free by `clean` (UAF), no
`first_valid` past an insert in flight (FV), and no loss (LOSS: a live value freed or stranded below
`first_valid`). The visited set is 64-bit hash compaction (collision odds about n^2/2^65, under
1e-5 at these sizes). Results in `.scratch/model/results*.txt`.

| Configuration | States | Verdict |
|---|---|---|
| main, sched / cont, 2 slots | 798,949 / 831,991 | no violation |
| main, sched, 1 slot (overflow) | 1,775,052 | no violation |
| main mutants: no cap, no announce, no floor raise, every-call refresh (cont), no overflow cap | 5,494-9,233 | FV, FV, LOSS, LOSS, FV |
| window, sched / cont, 1 slot, width 2 | 17,432,395 / 18,391,346 | no violation |
| window, 2 slots / period 2 / width 1 / width 3 with leads 1,4 and 6 ticks | 4,976,780 / 5,387,062 / 14,640,559 / 28,695,298 | no violation |
| window without the epoch count | 274,647 | FV: first_valid 3 passed key 2 |
| window without the `W` recheck | 377,107 | LOSS: key 2 stranded below first_valid 4 |
| window without the previous-window hold | 376,339 | FV |
| window without the advance gate (width 1) | 117,241 | FV |
| window without the cap at `W` (width 1) | 1,289,750 | UAF: reserve in a freed node |
| window with an immediate shrink (width 2 / 3) | 265,748 / 1,593,083 | FV |

The stall longer than the window is covered: in the passing runs a far producer stays in flight
while the window moves up to 9 times past the `W` it read and the walk floor passes its key by up to
4. A non-monotone shrink mutant has no effect: a move happens only once the floor is within a chunk
of `W` and the narrow width is one chunk, so the new `W` is always above the old one. Shrinking is
safe by construction; the mutant that loses is one that pulls `W` back at once.

### Throughput and p99 (`bench_structures`, stash rows)

Best of 3 per cell per round, 5 interleaved rounds; median [min-max] over rounds.

| Keys | Threads | main Mops | win Mops | Δ | main p99 ns | win p99 ns | Δ p99 |
|---|---|---|---|---|---|---|---|
| spread | 1 | 1.07 [1.04-1.07] | 1.07 [1.05-1.10] | +0.2% | 1507 [1491-1560] | 1499 [1490-1532] | -0.5% |
| spread | 2 | 1.45 [1.44-1.49] | 1.49 [1.47-1.53] | +2.6% | 2161 [2106-2200] | 2117 [2066-2173] | -2.0% |
| spread | 4 | 2.04 [1.95-2.07] | 2.07 [1.92-2.13] | +1.6% | 3231 [3193-16290] | 3129 [3055-18779] | -3.2% |
| spread | 8 | 2.68 [2.63-2.84] | 2.81 [2.72-2.91] | +5.0% | 4564 [4414-4643] | 4479 [4421-4614] | -1.9% |
| spread | 14 | 3.80 [3.69-3.90] | 4.01 [3.77-4.05] | +5.6% | 6620 [6496-6852] | 6667 [6245-6960] | +0.7% |
| convergent | 1 | 1.89 [1.79-2.03] | 1.98 [1.93-2.08] | +4.5% | 719 [670-755] | 696 [665-709] | -3.2% |
| convergent | 2 | 2.32 [2.15-2.40] | 2.37 [2.24-2.43] | +2.1% | 1452 [1432-1477] | 1434 [1425-1451] | -1.2% |
| convergent | 4 | 4.16 [4.09-4.27] | 4.13 [4.09-4.27] | -0.6% | 2090 [1651-2109] | 1750 [1605-2086] | -16.3% |
| convergent | 8 | 5.89 [5.52-5.91] | 5.86 [5.55-5.92] | -0.5% | 2382 [2366-2625] | 2424 [2370-2590] | +1.8% |
| convergent | 14 | 7.45 [6.98-7.70] | 7.65 [7.14-7.71] | +2.6% | 3249 [3031-3355] | 3191 [3157-3472] | -1.8% |

A/A anchor (main against a copy of itself, 3 rounds, same method, `.scratch/logs/aa-summary.md`):
throughput deltas from -2.4% to +2.2%, p99 from -3.6% to +3.1%, apart from the 4-thread spread p99,
which is bimodal on both. The variant's +5.0% and +5.6% at 8 and 14 spread threads exceed that,
but main's own 14-thread cell ran 3.98 to 4.06 in the anchor session, as high as the variant here,
so this is no regression and possibly a small gain, not a claimed win. No cell loses beyond noise.

### Many mostly-idle producers (`bench_bursty`, 2 s, bursts of 8, 0.5-2 ms naps, keys now + 0-100 ms)

5 interleaved rounds (the A/B):

| Threads | Build | adds/s | p50 ns | p99 ns | p999 ns | overflows (sum) | far (sum) | adds (sum) | lost |
|---|---|---|---|---|---|---|---|---|---|
| 256 | main | 1311628 [1306650-1326568] | 1207 | 11652 [8730-20895] | 32760 [27440-42039] | 39 | 0 | 13134428 | 0 |
| 256 | win | 1312581 [1303705-1318925] | 1189 | 11079 [8599-18438] | 29612 [29321-42424] | 0 | 8736876 | 13111866 | 0 |
| 1024 | main | 3290793 [3242303-3530350] | 2809 | 7915 [7031-9153] | 40926 [29934-44412] | 104 | 0 | 33541020 | 0 |
| 1024 | win | 3400955 [3268380-3446595] | 2784 | 7752 [7672-8784] | 40964 [36484-44205] | 0 | 33400384 | 33694236 | 0 |

Plus 10 more interleaved rounds at 1,024 threads for the overflow count
(`.scratch/logs/overflow-summary.md`). Overflow, all 1,024-thread runs:

| Build | Runs | Adds | Runs that overflowed | Overflowed claims | Per million adds |
|---|---|---|---|---|---|
| main | 15 | about 103M | 6 (11, 25, 39, 41, 104, 113) | 333 | 3.2 |
| window | 15 | about 103M | 0 | 0 | 0 |

At 256 threads main overflowed once (39 claims in about 13M adds) and the window never did. Main's
rate here is higher than the 2 in about 35M quoted from the earlier run; its overflows come in
bursts within a run. With the window, 99% of 1,024-thread inserts take the far path and the board
holds at most 9 to 19 keys at a scan (probe below). Adds/s and p50/p99/p999 are within the rounds'
spread of main.

### Window state (`.scratch/probes/bursty_probe.cc`)

| Build | Threads | Walks | Narrow walks | Far-pinned walks | Board occupancy mean / max |
|---|---|---|---|---|---|
| window | 256 | 8,100 | 0 | 38 | 0.54 / 19 |
| window | 1,024 | 187 | 0 | 2 | 0.68 / 9 |
| always narrowing (narrow at 1 slot taken, widen at 0) | 256 | 8,840 | 35 | 93 | 0.47 / 13 |
| always narrowing | 1,024 | 174 | 3 | 3 | 0.94 / 9 |

The adaptive shrink never engaged at the default thresholds (narrow at 32 slots taken): nothing
here came near. A far pin (the previous epoch still had a far insert in flight at a scan) held
`first_valid` on 0.5% of walks at 256 threads, each released by the next scan.

### Starved stress (48 nice'd busy loops, 4 producers, 2 s, 40 interleaved runs per binary)

| Lead | main | window |
|---|---|---|
| 0 ms (52% of inserts far) | 0 failed, 0 lost of 101,254,237 | 0 failed, 0 lost of 101,498,262 |
| 60 ms (99% far, straddling `W`) | 0 failed, 0 lost of 105,505,213 | 0 failed, 0 lost of 106,291,187 |
| always-narrowing build, 20 runs at 0 / 60 ms | | 0 lost of 51,459,437 / 52,568,371 |

### Sanitizers

| Run | Result |
|---|---|
| `test.cc`, TSan and ASan+UBSan | pass |
| `concurrent` TSan, 4 producers, lead 0 and lead 60, 5 runs each | 0 warnings, every task fired once (about 7.2M) |
| `concurrent` ASan+UBSan, 16 producers, lead 0 and lead 60, 5 runs each | 0 errors, every task fired once (about 53.4M) |
| always-narrowing `concurrent` TSan, 5 runs | 0 warnings, every task fired once (about 4.5M) |
| CI (`gh workflow run`, run 39381632466) | green on linux x86_64 gcc and clang, linux arm64 gcc, linux TSan, linux ASan+UBSan, macOS x86_64 and arm64, FreeBSD x86_64 |

`test.cc` gains a deterministic far-insert stall check; the C++ mutants without the epoch count,
without the previous-window hold, and without the advance gate each fail it or the in-flight check.

### Memory and the other axes

`sizeof(StashContext)` stays 4,352 B: the two epoch counts fit in each board line beside the key,
`atom_window` shares the insert floor's line, and the consumer fields fit on line 1. No disk or
indexing axis applies. The consumer's scan reads the previous epoch's count on each of the 64 lines
it already reads.

## Verdict

The model holds, every mutant loses, the sanitizers and both stress modes are clean, and nothing
regresses beyond the anchor's noise. Overflow at 1,024 bursty threads goes from 3.2 per million adds
to none measured. It meets the owner's bar to land, with two caveats for the reviewers: overflow on
main is a bounded, non-sticky freeze of `first_valid`, so the win is in tail cleanup delay, not in
correctness or throughput; and the shrink never engaged on these workloads, so it is safe (model,
always-narrowing stress) but its benefit is unmeasured. Not measured on Linux or arm64 beyond CI.

## Fixed width (owner's decisions, d234d24)

The owner kept the board and dropped the adaptive shrink: the window is always `WINDOW_CHUNK *
WINDOW_WIDE` finest slots (64 ms on Xapiand's wheel). In the same change, overflow and holds are
reported only through compile-time hooks (`STASH_HELD_STAT(overflow, far_pin)` beside
`STASH_ANNOUNCE_STAT` and `STASH_FAR_STAT`), `add` returns false instead of throwing past the
horizon, and `next` is `noexcept` for nothrow value types. None of these touch the handshake, so
the expectation was no difference against the first round; this section re-measures.

### Model

`.scratch/model/stash_model.py` minus the width and occupancy state (the adaptive model is kept as
`stash_model_adaptive.py`); `run_fixed.sh`, results in `results-fixed.txt`.

| Configuration | States | Verdict |
|---|---|---|
| fixed, sched / cont, 1 slot, width 2 | 10,094,389 / 10,590,992 | no violation |
| fixed, 2 slots / period 2 / width 1 / width 3 with leads 1,4 and 6 ticks | 3,289,315 / 3,376,081 / 10,578,023 / 17,164,744 | no violation |
| without the epoch count (width 3) | 203,049 | FV: first_valid 3 passed key 2 |
| without the `W` recheck (width 3) | 273,888 | LOSS: key 2 stranded below first_valid 4 |
| without the previous-window hold (width 3) | 276,232 | FV |
| without the cap at `W` (width 1) | 999,604 | UAF: reserve in a freed node |
| without the advance gate (width 1) | 80,486 | FV |
| main, sched / cont, 2 slots | 798,949 / 831,991 | no violation |

A far producer stays in flight while the window moves up to 9 times past the `W` it read. The
shrink mutants are gone with the shrink. The model does not cover the horizon, so the bool `add`
needs no model run: a refused key returns before `put`, exactly where the throw left.

### A/B against main

Same method as above (best of 3 per cell per round, 5 interleaved rounds, median [min-max]), under
one strict `quiet run` hold; another agent's light single-thread hold was live throughout. Both
builds compile the identical harness, a snapshot in `.scratch/probes/ab-src/` (its `try`/`catch`
around `add` compiles against either API). Logs: `.scratch/logs/ab-fixed.log`, summary
`ab-fixed-summary.md`.

| Keys | Threads | main Mops | fix Mops | Δ | main p99 ns | fix p99 ns | Δ p99 |
|---|---|---|---|---|---|---|---|
| spread | 1 | 1.02 [0.95-1.06] | 1.00 [0.93-1.08] | -2.1% | 1636 [1583-1930] | 1729 [1503-2020] | +5.7% |
| spread | 2 | 1.42 [1.39-1.48] | 1.41 [1.34-1.50] | -0.8% | 2277 [2168-2327] | 2318 [2115-2403] | +1.8% |
| spread | 4 | 1.99 [1.97-2.06] | 1.97 [1.91-2.07] | -1.4% | 3262 [3070-3299] | 3348 [3186-3560] | +2.6% |
| spread | 8 | 2.59 [2.48-2.78] | 2.65 [2.46-2.84] | +2.4% | 4590 [4404-4617] | 4528 [4396-4762] | -1.4% |
| spread | 14 | 3.73 [3.45-3.98] | 3.61 [3.35-3.93] | -3.2% | 6565 [6130-6891] | 6491 [6422-7006] | -1.1% |
| convergent | 1 | 1.88 [1.84-2.01] | 1.90 [1.86-1.99] | +1.4% | 780 [686-795] | 771 [703-797] | -1.2% |
| convergent | 2 | 2.29 [2.29-2.39] | 2.31 [2.26-2.37] | +0.7% | 1464 [1436-1539] | 1483 [1444-1562] | +1.3% |
| convergent | 4 | 4.16 [4.12-4.22] | 4.17 [4.12-4.29] | +0.3% | 1924 [1746-2167] | 1842 [1649-1977] | -4.3% |
| convergent | 8 | 5.79 [5.42-5.86] | 5.78 [5.48-5.79] | -0.2% | 2510 [2414-2657] | 2548 [2449-2675] | +1.5% |
| convergent | 14 | 7.15 [6.62-7.70] | 7.40 [6.42-7.65] | +3.4% | 3151 [3083-3166] | 3229 [3045-3280] | +2.5% |

| Threads | Build | adds/s | p50 ns | p99 ns | p999 ns | overflows (sum) | far (sum) | adds (sum) | walks (sum) | held by overflow / far pin (sum) | lost |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 256 | main | 1448205 [1419204-1450825] | 1445 | 16911 [11342-18567] | 33699 [31833-56583] | 5 | 0 | 14429662 | 32841 | n/a | 0 |
| 256 | fix | 1449768 [1438242-1454379] | 1382 | 12985 [7386-20543] | 34510 [23697-50952] | 0 | 9657379 | 14483912 | 34041 | 0 / 2 | 0 |
| 1024 | main | 3318098 [3134067-3360214] | 2781 | 12000 [11027-12313] | 39345 [30376-42179] | 0 | 0 | 32608294 | 924 | n/a | 0 |
| 1024 | fix | 3267548 [3203384-3321334] | 2801 | 11910 [11634-12450] | 35659 [34022-42719] | 0 | 32307767 | 32618750 | 915 | 0 / 3 | 0 |

No difference beyond the run-to-run spread: every throughput and p99 cell's ranges overlap, the
throughput deltas run from -3.2% to +3.4% (the A/A anchor gave -2.4% to +2.2% in a quieter
session), and no cell shows the same direction at both spread and convergent keys. Bursty adds/s
and latency percentiles match. Overflow: main overflowed 5 claims at 256 threads in one round,
the fixed window none; 99% of 1,024-thread inserts and two thirds of 256-thread inserts took the
far path. `STASH_HELD_STAT` counted 2 and 3 walks held by a far pin (of about 34,000 and 900
walks) and none by overflow.

### Starved stress and sanitizers

| Run | main | fixed |
|---|---|---|
| Starved, lead 0, 40 runs | 0 failed, 0 lost of 101,768,395 | 0 failed, 0 lost of 103,268,551 |
| Starved, lead 60, 40 runs | 0 failed, 0 lost of 104,298,595 | 0 failed, 0 lost of 102,309,006 |
| `test.cc`, TSan and ASan+UBSan | | pass |
| `concurrent` TSan (4 producers) and ASan+UBSan (16), lead 0 and 60, 5 runs each | | every task fired once, no reports |

`sizeof(StashContext)` is still 4,352 B.

## Not built: per-slot writer counters

The first idea in this round counted writers on the slots being descended into, instead of the
board. The owner dropped it before a prototype (the board's uncontended writes beat RMWs on hot
slots), so there are no numbers; the design analysis is kept for the question it raised, which
level would hold the counter.

Only the outermost level can hold it alone. The walk skips a null child, a short or empty chunk
chain, and everything past `last_valid`, and lower nodes are created lazily, so a counter inside a
lower node either does not exist yet when the producer needs it or is never read by a walk that
skipped the node's parent. The outermost level's slots live in the wheel object itself and are read
for every range the walk passes, but they are 18 s wide on Xapiand's wheel: the walk could hold
`first_valid` only at the 18 s slot's start, and under steady load some producer is always inside
the current slot. Finer precision needs hand-over-hand counting down the levels (increment the
child's counter, then release the parent's), two RMWs per level on counters every producer in the
same 18 s, 500 ms and 50 ms span shares. The cost would have been 4 B per slot at every level (19.2
KB eagerly on the outermost 4,800-slot level).
