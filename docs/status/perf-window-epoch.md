# Status: perf/window-epoch

## State

Owner's decisions folded in, ready for the code review and the architect's re-check of the window
handshake; not merged. Results in `docs/perf/window-epoch.md` ("Fixed width" for the current code).

## Done

- Plan (db6a9bc); prototype, tests, bursty far column (170e1d8); width initializer (6af4922);
  first-round results with the adaptive shrink (5af109a, 8fd29e9, 63f3989).
- Owner's decisions (d234d24): fixed window width (adaptive shrink removed); overflow and holds
  reported only through compile-time hooks (`STASH_HELD_STAT`, beside `STASH_ANNOUNCE_STAT` and
  `STASH_FAR_STAT`), no production counters; `add` returns bool (false past the horizon, storing
  nothing; only `std::bad_alloc` throws); `next` (walk, peep, clean) `noexcept` whenever the value
  type's copy, bool tests and destructor are.
- Model on the fixed-width variant: no violation in 6 configurations (both walk styles, a far
  producer stalled across up to 9 window moves); every mutant still loses (no epoch count, no `W`
  recheck, no previous-window hold, no cap at `W`, no advance gate). main anchors pass.
- TSan and ASan+UBSan on `test.cc` and `concurrent.cc` (lead 0 and 60, 5 runs each): clean.
- Starved stress, 40 interleaved runs at lead 0 and 60: 0 failed, 0 lost on main and fixed.
- A/B against main, 5 interleaved rounds under a strict hold: no difference beyond the spread
  (throughput -3.2% to +3.4%, every range overlapping); overflow 5 claims on main at 256
  threads, none fixed; far-pin holds 2 and 3 walks, overflow holds 0.
- DW11 (b458364 plus the cursor retry): allocation before the leaf claim; tests, model
  (`results-abort.txt`), TSan and ASan+UBSan clean; the CAS claim's cost is under Blocked on.
- Every caller of `add` in the tree updated or confirmed (tests, longevity, demo, four benches).
  Downstream: Kronuz/scheduler checks the bool (perf/lazy-rearm eca6062); logger, Xapiand and
  Detent call no stash `add` directly.

## Next

1. Code review (stash and scheduler together) and the architect's re-check of the handshake.
2. A timed run on Linux and arm64 (CI covers correctness only).
3. After this lands: repin Kronuz/scheduler's stash from d234d24 to main.

## Blocked on

- Owner decision: the cost of the DW11 fix. Claiming the leaf slot by CAS after allocating
  (b458364, with the cursor retry) never leaves a hole, but on convergent keys (one hot leaf, as
  the logger's async lines and tasks due "now" are) it loses 4.2% at 4 threads, 5.8% at 8 and
  16.7% at 14, p99 +35% at 14 (`docs/perf/window-epoch.md`, "Cost of claiming by CAS"); spread
  and bursty are unaffected. Options:
  1. Keep the CAS claim (airtight; model and tests cover it).
  2. Keep the fetch-add claim, build the value box first (free), and on a failed storage
     allocation after the claim install a shared, never-freed drained chunk into the node, with
     producers that land in it claiming again. This closes a missing chunk but not a missing chain
     node, so it is not airtight without more design.
  3. Measure on Linux and arm64 first: the cost is CAS contention on x86 here.
  Recommendation: 1 until 2 is designed and model-checked; the architect's re-check should see
  this.

## For reviewers

- DW11 is fixed (b458364): `StashValues::put` builds the value box and materializes the slot's
  chunk before claiming it, so `std::bad_alloc` reserves nothing. `test_alloc_failure` fails each
  site through `STASH_ALLOC_HOOK` and fails on the previous `put`; the model's HOLE invariant
  holds with allocation failures before the claim and catches a failure after it.
