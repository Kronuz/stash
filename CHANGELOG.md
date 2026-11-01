# Changelog

All notable changes to `stash` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Earlier changes are recorded
in [STRAND_FIX.md](STRAND_FIX.md) and `docs/perf/`.

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
