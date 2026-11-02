# Claim hint: does main still cost depth-64 p99, and a rare publish

> Status: IN PROGRESS (2026-11-02). Workstream perf/claim-hint-publish (stash#1). Machine: Intel
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

Each is `stash.h` with a different publish; screened on depth 64 only (`.scratch/screen.sh`, the
stash rows of `bench_steady` at depth 64, 250 ms per row, best of two), interleaved with START, main
and START again.

1. Rare release-store publish after the fill, K = 4 and K = 2 (`-DEXP_PUBLISH_K`). A claim past the
   head's eight chunks publishes only when no hint exists or the hint is K or more nodes behind its
   own, with a plain release store after the fill. Hypothesis: at depth 64 a leaf of 120 values
   publishes once or twice, under 1% of the inserts, so the publishes leave the p99 and the compare
   exchange leaves the claim path; the cost is up to K - 1 extra hops for claims behind the hint.
   Keep if depth-64 p99 is within the A/A band of START at t=4 and t=14 and the chain-walk rows
   (`test_claim_walk`, depth 1 024 and 16 384, `bench_structures`) lose under 3%. Remove otherwise.

The walker publishing the hint is not tried here: the walker reaches a leaf when its slot is due,
after the producers have filled it, so a claim in a leaf that is still filling gains nothing from it
(stash#1 measured 0.43 to 0.71 on `bench_structures` A-spread for it).

## Results
