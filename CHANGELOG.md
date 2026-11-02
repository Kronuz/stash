# Changelog

All notable changes to `stash` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Earlier changes are recorded
in [STRAND_FIX.md](STRAND_FIX.md) and `docs/perf/`.

## 2026-11-01

### Changed

- An insert into a leaf's first eight chunks (`claim_hint_chunks`, 80 values at `Size`
  10) walks its chain from the head again, as before 2026-10-31, and neither reads nor
  publishes the claim hint; past them it resumes at the hint as before. The hint had every
  insert read it and every insert that reached a new chunk publish it with a
  compare-exchange on the leaf's contended line, and `bench_steady`'s depth-64 rows paid
  for it at p99. With 4 and 14 producers: 979 and 1 771 ns before
  the hint, 1 125 and 1 979 ns with it (Apple M4 Pro, 4 rounds); 714 and 1 280 ns before,
  750 and 1 365 ns with it (Intel i9-9980HK, 3 rounds); `bench_steady 250`, both builds
  interleaved in one strict hold per machine. Leaves of more values keep the linear walk,
  `n / Size + 28 Size` nodes for `n` values (`test_claim_walk`: 1 111 steps for 4 000
  inserts into chunks of 4, against 1 998 000 from the head each). Results and orders are
  unchanged.
- This does not bring the depth-64 rows back to the code before the hint. Those leaves
  hold about 120 values, not 64 (a finest slot drains whole, so a value waits about half a
  millisecond): 34.6% of their inserts land past the first eight chunks and still read and
  publish the hint (counted through `STASH_CLAIM_HINT_HOOK`, M1 Pro). Measured in a second
  strict hold per machine (`bench_steady 250`, p99 ns, before the hint and after this
  change, 4 and 14 producers): Apple M4 Pro 896 to 979.5 (0.898) and 2 270.5 to 2 229.5
  (1.018), 4 rounds; the stash rows alone, 6 rounds, 958 to 1 020.5 (0.938) and 1 729 to
  1 833.5 (0.943), where the hint as it was gave 0.938 and 0.922. Intel i9-9980HK 696 to
  707 (0.972) and 1 259 to 1 303 (0.963), 3 rounds; the stash rows alone, 5 rounds, 0.961
  and 0.942, the hint as it was 0.962 and 0.920. Against the commit before (e50bf9a), on the
  rows the hint won: geomeans of `bench_scheduler`, `bench_bursty` and `bench_structures`
  1.000, 1.003 and 0.983 (M4 Pro) and 1.012, 0.979 and 0.993 (i9); stash rows below 0.97
  beyond their A/A spread, all on the i9 but one: `structures A-spread stash` t=1 insert
  0.918, t=4 insert 0.940, t=1 p99 0.946, `B-convergent stash t=1 p99` 0.925,
  `bursty threads=256 add` p99 0.898 and p999 0.877, `steady D=16384 stash t=1 p99` 0.940,
  and on the M4 Pro `structures A-spread stash t=8 p99` 0.799 (2 and 3 rounds;
  [stash#1](https://github.com/Kronuz/stash/issues/1)).
- The cause, from variants of `bench_steady`'s stash rows in the same holds (p99 ratio
  against the code before the hint, 4 and 14 producers, M4 Pro): the hint as it was,
  0.919 and 0.979; without the compare-exchange on a leaf's first claim, 0.920 and 0.941;
  with a release store instead of the compare-exchange, 0.919 and 0.980; with a relaxed
  store, 1.000 and 1.022, but a relaxed store does not order the node's index for the
  reader; the code before the hint with the hint's 16 bytes added, 1.000 and 1.000. The
  ordered write that publishes the hint is the cost, not the first claim or the size. A
  candidate that skipped the hint in the head's chunk alone and published it with a release
  store measured 0.940 and 0.934 on the M4 Pro (0.988 and 1.009 on the i9) and was not
  kept.

### Added

- `STASH_CLAIM_HINT_HOOK(publish)`, a no-op unless defined, runs for every read and
  publish of a leaf's claim hint; `test_claim_walk` requires none in the first eight
  chunks.
- `test/concurrent.cc` checks that every id fires exactly once (a lost or duplicated
  claim fails it) and takes a depth cap and a key band; CTest `stash_claims_d64` runs 14
  producers held at 64 pending tasks over a 2 ms band.

## 2026-10-31

### Changed

- An insert into a leaf resumes its chain walk at the furthest node any claim into that
  leaf has resolved (`atom_claim_hint`) instead of the leaf's head. Filling a leaf with
  `n` values stepped over about `n^2 / (2 Size)` nodes, quadratic in the number of values
  that share a finest slot: a burst of keys due in one millisecond, or a backlog of
  overdue keys, which are all filed at the floor's leaf. It now steps over about
  `n / Size` (`test_claim_walk`: 4 000 inserts into one leaf, 1 000 steps against
  1 998 000). `test/concurrent.cc` (four producers, 2 s, keys due now) inserts 5.8 to 7.7
  million values where it inserted 3.0 to 3.6 million (Linux x86_64 VM, clang 12 -O2,
  five runs each). Through the scheduler, a TSan build that posts 200 000 tasks due
  within 2 ms from four threads runs in 4.4 s every time (it took 5.6 to 31 s and timed out
  one run in three). API, results and orders are unchanged; a leaf grows by 16 bytes and
  each chain node by 8.

### Documented

- TSan from LLVM 12 or older reports races with two or more producers that are not
  there (AGENTS.md, Traps).
