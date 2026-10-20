# Status: perf/window-epoch

## State

Prototype built (170e1d8) with the adaptive shrink; model holds, every mutant loses; tests and
sanitizers clean. Timed A/B against main pending a strict hold.

## Done

- Plan (db6a9bc); prototype, tests, bursty far column (170e1d8).
- Model (`.scratch/model/stash_model.py`, results in `results-window*.txt`): main and the variant
  hold in both walk styles; the far producer is stalled past the window (the window moved up to 9
  times past it, the floor up to 4 past its key); mutants without the epoch count, the W recheck,
  the previous-window hold, the advance gate, the window cap, or with an immediate shrink lose.
- TSan and ASan+UBSan: test.cc, concurrent.cc at lead 0 and lead 60 (far path), all clean.

## Next

1. Strict-hold A/B: bench_structures, bench_bursty (+ overflow rounds, window probe), starved
   stress at lead 0 and 60, 40 runs each.
2. Results and recommendation in docs/perf/window-epoch.md.

## Blocked on

Nothing (waiting for the host's light hold to clear before the strict runs).
