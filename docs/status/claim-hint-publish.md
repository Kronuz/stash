# Status: perf/claim-hint-publish

> Status: CURRENT (2026-11-02) Settles Kronuz/stash#1. Results: `docs/perf/claim-hint-publish.md`.

## Done criteria

- A strict interleaved A/B (START 2e2b75f, main 8fa5d2a, START again as the A/A control) on the four
  benches with p50 and p99 per row.
- A publish whose depth-64 p99 is at START on the i9, with TSan and ASan clean, the existing tests,
  mutants, and the chain-walk rows no more than 3% slower than main.

## State

Main costs the i9 5% (t=4) and 6% (t=14) at depth-64 p99 (84 rounds; A/A 1.00). The candidate (one
release store per chunk after the fill, no threshold) is at START: 0.989 / 0.995. Five experiments
recorded; four losers removed. The M4 Pro and Linux are not measured; the lead lands the branch on
main, closes the issue and measures the other platforms.

## Done

- Ledger and plan: stash:6ef4585, stash:012f373. Candidate and test: stash:de4c3d5. Results: stash:b78f43e.
- 

## Next

- The lead: land perf/claim-hint-publish, close stash#1, measure the M4 Pro and Linux once.

## Blocked on

Nothing.
