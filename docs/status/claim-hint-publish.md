# Status: perf/claim-hint-publish

> Status: CURRENT (2026-11-02) Settles Kronuz/stash#1: does main (8fa5d2a, `claim_hint_chunks` = 8)
> still cost depth-64 p99 against the code before the hint (START 2e2b75f). Results in
> `docs/perf/claim-hint-publish.md`.

## Done criteria

- A strict interleaved A/B (START, HEAD, START again as the A/A control) on `bench_steady`,
  `bench_scheduler`, `bench_bursty`, `bench_structures`, with p50 and p99 per row.
- If HEAD is inside the A/A band at depth 64: README depth-64 cells from the run, comment on #1,
  close it. Otherwise at most five publish experiments; a win lands with TSan and ASan clean,
  the existing tests, a mutant, CI green.

## State

Starting. Host: i9-9980HK, Apple clang 17, macOS. The M4 Pro is not reachable from this host, so
its rows are not remeasured here.

## Done

## Next

## Blocked on
