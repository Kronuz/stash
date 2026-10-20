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
- DW11, fail-fast: value box before the fetch-add claim, terminate after it, `stash_result`
  from a `noexcept` `add`; tests, model (`results-abort.txt`, `results-failfast.txt`), TSan and
  ASan+UBSan clean; A/B against the fetch-add build within noise.
- Every caller of `add` in the tree updated or confirmed (tests, longevity, demo, four benches).
  Downstream: Kronuz/scheduler checks the bool (perf/lazy-rearm eca6062); logger, Xapiand and
  Detent call no stash `add` directly.

## Next

1. Code review (stash and scheduler together) and the architect's re-check of the handshake.
2. A timed run on Linux and arm64 (CI covers correctness only).
3. After this lands: repin Kronuz/scheduler's stash from d234d24 to main.

## Blocked on

Nothing. The owner chose fail-fast over the CAS claim's cost.

## For reviewers

- DW11 is fixed fail-fast: `add` is `noexcept` and returns `stash_result`; the value box comes
  before the fetch-add claim (a failure there is `no_memory`, nothing claimed), and a chunk or
  node failure after the claim terminates with "stash: out of memory after claiming a slot".
  `test_alloc_failure` checks every site in a forked child; the model's HOLE invariant holds and
  its abort-after-claim mutant still reaches HOLE. The CAS claim (b458364, e359978) was measured
  and replaced: up to 16.7% slower on one hot leaf, against no measurable cost now.
