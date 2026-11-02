# Status: fix/claim-hint-d64

> Status: CURRENT (2026-11-01) The no-slower fix for the depth-64 `bench_steady` p99 rows that
> `78a18f3` (the claim hint) slowed; gate file `campaign/gate/stash.md`, "Rows < 0.97".

## Done criteria

- `steady D=64 stash t=4 p99` and `t=14 p99` at least as fast as START (2e2b75f) on the M4 Pro
  and the i9: ratio >= 0.97 and outside the A/A spread, or better.
- No other row of the four benches more than 3% slower than HEAD (e50bf9a).
- The chain-walk win kept: `test_claim_walk` at n / 4 steps; the depth-1 024 and 16 384 rows.
- The concurrent tests clean under TSan; a stress at depth 64 with 14 producers that fails on a
  lost or duplicated claim.

## State

Candidate 528ccef pushed; strict holds queued on the M4 Pro and the i9 (one each, building
inside). CI dispatched on the branch (run 41367951740).

## Done

- Variants (`bench_steady`'s stash rows at depth 64 and 1 024, same code, screen on the M1 Pro
  under a light hold, 6 and 12 rounds): HEAD, no compare-exchange for a claim that resolved the
  head (`nofirst`), the publish as a relaxed store (`relaxed`) and as a release store (`rel`),
  START with HEAD's sizes (`pad`), and candidates. Too noisy at t=4 to rank (A/A 0.87 to 1.08,
  ranges 0.5 to 1.7); at t=14 HEAD 0.918, `relaxed` 0.957, the store-based candidate 0.969.
  The M4 and i9 holds repeat the variants.
- 528ccef: claims in the head's chunk read and publish nothing; a claim past the hint publishes
  with a release store. `STASH_CLAIM_HINT_HOOK`; `test_claim_walk` counts reads (n - 4) and
  publishes (n / 4 - 1). Revert check (HEAD's logic with the hooks at its load and successful
  compare-exchange): aborts, "read the hint 4 times and published it 1 times" in the head's
  chunk. Mutants caught: `>` to `>=` in the publish test (published 4 times), the load guard
  removed (read 4 times).
- `test/concurrent.cc`: every id must fire exactly once; depth cap and key band arguments; CTest
  `stash_claims_d64` (14 producers, depth 64, 2 ms band). Fails on a claim made by load and
  store instead of fetch-add, and on a hint base one chunk too far (lost ids, 2 runs each).
- M1 Pro, Apple clang 21: ctest 4/4; TSan `test.cc`, `concurrent.cc` with CI's arguments x3 and
  at depth 64 x3, 0 reports; ASan+UBSan `test.cc`, `concurrent.cc` CI x1 and depth 64 x2,
  clean; every id fired once.

## Next

- Read the holds; land on main if the rows pass, else the fallback (brief: START's behavior on
  that path, an issue with what was learned).
- CHANGELOG, README Performance rows that move, a lesson in campaign/LESSONS.md.

## Blocked on

Nothing.
