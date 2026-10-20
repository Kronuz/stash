# Status: perf/window-epoch

## State

Done, ready for review; not merged. Results and verdict in `docs/perf/window-epoch.md`.

## Done

- Plan (db6a9bc); prototype, tests, bursty far column (170e1d8); width initializer (6af4922).
- Model: main and the variant hold in both walk styles with a far producer stalled past the
  window; every mutant loses (no epoch count, no W recheck, no previous-window hold, no advance
  gate, no cap at W, immediate shrink).
- A/B against main, 5 interleaved rounds, plus an A/A anchor: no regression beyond noise.
- Overflow at 1,024 bursty threads: main 333 claims in about 103M adds (6 of 15 runs), window 0.
- Starved stress, 40 runs each at lead 0 and 60: 0 lost on both; always-narrowing build too.
- TSan, ASan+UBSan clean; CI green on all 8 jobs (run 39381632466, at 5af109a).

## Next

1. Code review and an architect re-check of the handshake before anything lands.
2. A timed run on Linux and arm64 (CI covered correctness only).

## Blocked on

Nothing.
