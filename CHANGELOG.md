# Changelog

All notable changes to `stash` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Earlier changes are recorded
in [STRAND_FIX.md](STRAND_FIX.md) and `docs/perf/`.

## 2026-11-01

### Changed

- An insert into a leaf's first eight chunks (`claim_hint_chunks`, 80 values at `Size` 10)
  walks its chain from the head again, as before 2026-10-31, and neither reads nor publishes
  the claim hint; past them it resumes at the hint as before. The hint made every insert
  read it and every insert that reached a new chunk publish it with a compare-exchange on the
  leaf's contended line, and `bench_steady`'s depth-64 rows (about 64 values a leaf) paid for
  it at p99: with 4 and 14 producers, 979 and 1 771 ns before the hint, 1 125 and 1 979 ns
  with it (Apple M4 Pro); 714 and 1 280 ns before, 750 and 1 365 ns with it (Intel
  i9-9980HK); `bench_steady 250`, START and HEAD interleaved in one strict hold, 4 and 3
  rounds. Those leaves now run the code they ran before the hint. A leaf of more values walks
  `n / Size + 28 Size` nodes in all (`test_claim_walk`: 1 111 steps for 4 000 inserts into
  chunks of 4, 1 998 000 from the head each). Variants of the stash rows, same hold, each
  against START: the hint without the compare-exchange on a leaf's first claim stays slow
  (M4 0.920 and 0.941), START with the hint's 16 bytes is as fast as START (1.000 and
  1.000), a release store instead of the compare-exchange stays slow on the M4 (0.919 and
  0.980), and only a relaxed store, which would not order the node's index, is as fast as
  START (1.000 and 1.022). Skipping the hint in the head's chunk alone and publishing it with
  a release store gave 0.940 and 0.934 on the M4 Pro and 0.988 and 1.009 on the i9.
  Results and orders are unchanged.

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
