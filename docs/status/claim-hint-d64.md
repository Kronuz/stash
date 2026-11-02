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

The fallback (b0fce1c, landed as 4187183) does not meet the bar. A second strict hold per
machine timed it against START and e50bf9a: depth-64 p99 at t=4 / t=14, M4 Pro 0.898 / 1.018
(stash rows alone 0.938 / 0.943), i9 0.972 / 0.963 (0.961 / 0.942); details in CHANGELOG
2026-11-01 and stash#1. Its premise was wrong: those leaves hold about 120 values, so 34.6%
of their inserts still use the hint. Waiting on the lead.

## Done criteria

- `steady D=64 stash t=4 p99` and `t=14 p99` at least as fast as START (2e2b75f) on the M4 Pro
  and the i9: ratio >= 0.97 and outside the A/A spread, or better.
- No other row of the four benches more than 3% slower than HEAD (e50bf9a).
- The chain-walk win kept: `test_claim_walk` at n / 4 steps; the depth-1 024 and 16 384 rows.
- The concurrent tests clean under TSan; a stress at depth 64 with 14 producers that fails on a
  lost or duplicated claim.

## State

Fallback landed in place of the fix (issue Kronuz/stash#1): the store-published hint (528ccef) did not reach START on
the M4 Pro, so a leaf's first eight chunks run START's code again (b0fce1c) and the hint works
as in 78a18f3 past them. CI green on the branch before landing; GitHub issue filed with the
findings.

## Done

- One strict hold per machine (M4 Pro 16:54-17:03, i9 16:47-16:55; builds inside;
  `bench_steady 250` START/CAND/HEAD/START interleaved, 4 rounds M4, 3 i9). Named rows, p99 ns,
  START / HEAD / CAND: M4 t=4 979 / 1125 / 1041.5 (CAND 0.940), t=14 1771 / 1979 / 1875
  (0.934); i9 t=4 714 / 750 / 723 (0.988), t=14 1280 / 1365 / 1269 (1.009). CAND misses the
  bar on the M4.
- Variants (stash rows of `bench_steady` at depth 64 and 1 024, same code, 3 rounds, ratio
  against START at t=4 / t=14 p99). M4: HEAD 0.919 / 0.979, no first-claim CAS 0.920 / 0.941,
  release store 0.919 / 0.980, relaxed store 1.000 / 1.022, START + 16 bytes 1.000 / 1.000,
  CAND 0.959 / 0.980. i9: HEAD 0.973 / 0.933, no first-claim CAS 0.982 / 0.913, relaxed
  0.960 / 0.986, release store 1.015 / 0.991, START + 16 bytes 0.992 / 0.987, CAND 0.976 /
  0.994. Cause: the ordered write that publishes the hint (a CAS, or on the M4 a release
  store too); not the first claim, not the size. A relaxed store does not order the node's
  index, so it is not a fix.
- HEAD vs CAND, other benches (ab_compare, 3 rounds; i9 structures 2): geomeans M4 1.051
  scheduler, 1.047 bursty, 1.019 structures; i9 1.006, 0.981, 1.019; stash rows below 0.97
  sit inside the spread the control rows show (heap, Asio, multimap at 0.85 to 0.96).
- b0fce1c (fallback): `claim_hint_chunks` = 8; tests and checks below.
- 528ccef, superseded by b0fce1c: head chunk skips the hint, release-store publish.
- `test_claim_walk`: no hint access in the first 32 slots, n - 32 reads, n / 4 - 8 publishes,
  at most n / 4 + 120 steps (1 111). Revert check (78a18f3's logic, hooks at its load and
  successful CAS): aborts, 32 reads and 8 publishes in the first chunks. Mutant caught: the
  threshold `>=` to `>` (3 967 reads, want 3 968).
- `test/concurrent.cc`: every id fires exactly once; depth cap and key band; CTest
  `stash_claims_d64`. Fails on a claim by load and store instead of fetch-add, and on a hint
  base one chunk too far (lost ids, 2 runs each).
- M1 Pro, Apple clang 21, for both 528ccef and b0fce1c: ctest 4/4; TSan `test.cc`,
  `concurrent.cc` CI args x3, depth 64 x3: 0 reports; ASan+UBSan clean; every id fired once.
- The fallback was not timed in a strict hold (one per machine, spent on the candidate). Its
  depth-64 path is START's code (no hint access in the first 80 slots, which the test pins)
  with HEAD's layout, which both holds measured at START (1.000 / 1.000 M4, 0.992 / 0.987
  i9). An M1 Pro light screen (13 rounds) has it with HEAD on the depth-1 024 rows; its t=4
  rows there are noise (A/A ranges 0.4 to 1.6).

## Next

- The lead decides: a fix that keeps the publish off the claim path for short chains (stash#1
  lists candidates), a threshold fitted to about 120 values, or the full revert of 78a18f3.

## Blocked on

Nothing.
