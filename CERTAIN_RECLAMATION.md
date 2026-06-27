# Certain reclamation (Path B) — design

Goal: make `stash` correct under **arbitrary** concurrency — no lost or
double-fired entries, no use-after-free, no unbounded growth — with **no time
margin**. The current `clean` is correct only inside a usage envelope (see the
README); this replaces the wall-clock margin with a proof.

## What breaks today, precisely

Three failure modes, all exposed by hammering the structure outside the envelope
(see `test/concurrent.cc`):

1. **Boundary use-after-free (R1).** A producer descending for key `K` loads a
   child pointer `C` for `K`'s slot; the clock crosses that slot; the consumer
   frees `C`. The producer then dereferences freed memory. (Margin 0 → corruption
   / infinite loop under ASan.)
2. **Reacquisition / ABA (R2).** Physical slots are reused every wheel period
   (the modulus). A producer scheduling near the horizon writes physical slot `s`
   for the *next* period while the consumer frees `s`'s *old* subtree — same
   atomic pointer, concurrent free + acquire.
3. **Bounds-ordering strand.** `add` puts the task, then bumps
   `atom_last_valid_key`. A walk in that window stops short of the task; if
   `first_valid` then advances past the slot, the task is stranded (never fired,
   never reclaimed). Independent of `clean`; shows up only with many producers.

The 1-minute margin is a wall-clock approximation of "no producer is still in
here." It is not a guarantee (a preempted thread breaks it) and it does not cover
R2 or the strand at all.

## The mechanism: producers announce their in-flight key

A single mechanism fixes all three. Each producer, for the whole duration of an
`add`, publishes the key it is working on in a per-thread slot:

```
announce[tid] = key;   // release, on entry to add()
... descend + write ...
announce[tid] = INF;   // on exit
```

Define `safe_floor = min over active producers of announce[]` (`INF` if none).
Every active producer is provably working at a key `>= safe_floor`.

### Fix for the strand: clamp the walk

The consumer advances `first_valid` (and fires) only up to `min(now, safe_floor)`
— never past a key a producer is still inserting. So a task being inserted at `K`
keeps `first_valid <= K` until that producer finishes; the walk cannot pass it.
Inserts are microseconds, so the walk stalls at most that long. This removes the
strand by construction.

### Fix for R1 + R2: hazard-scan before free (margin → 0)

To free a top subtree at physical slot `s` (its window entirely below
`safe_floor`, so no producer is *below* it):

1. `C = atom_ptr[s].exchange(nullptr)` (acq_rel) — remove it from the tree first,
   so any producer that loads `atom_ptr[s]` *after* this sees `null` and creates a
   fresh child, never touching `C`.
2. Scan `announce[]` (acquire). For each active `K`, compute `top_slot(K)`. If any
   equals `s`, some producer may hold `C` → **retire** `C` (defer). Else **free**
   `C` now.
3. Drain the retire list periodically: free a retired `C` once no announce maps to
   its slot.

`safe_floor < window(s)` covers R1 (no producer is in or below `s`). The
`top_slot(K) == s` scan covers R2 (a producer one period ahead, large `K`, that
hashes back onto physical `s`). Checking at the top-slot granularity protects the
whole subtree, because any producer anywhere inside subtree `s` announced a key
with `top_slot == s`.

### Why the ordering is correct

Producer: `announce = K` (release) **happens-before** it loads `atom_ptr[s]`.
Consumer: `exchange(nullptr)` (acq_rel) **happens-before** it scans `announce`
(acquire). Two cases for a producer that ends up holding `C`:

- It announced before the consumer scanned → consumer sees the announce → retires
  `C` → safe.
- It announced after the consumer scanned → its load of `atom_ptr[s]` (after its
  announce, hence after the consumer's exchange) returns `null` → it never touches
  `C`.

So a freed subtree is provably unreferenced. No wall-clock anywhere.

## Implementation plan (incremental, harness-gated)

Each step must keep `test/test.cc`, `test/longevity.cc`, and
`test/concurrent.cc` (margin 0) green, the last under **ASan and TSan**.

1. **Announce registry.** A fixed-size `std::array<std::atomic<ull>, N>` reachable
   by both producers and the consumer (in `StashContext`), plus a thread-local
   index assigned from an atomic counter on first `add`. `INF` = idle.
2. **Producer announce.** Wrap the top-level `add` with set/reset of `announce`.
   Memory order: release on set.
3. **Walk clamp.** Bound `first_valid` advance by `safe_floor`.
4. **Hazard free.** Replace the time-cutoff drop in `clean` with exchange → scan →
   retire/free, plus a retire list drained each `clean`.
5. **Drop the margin.** `clean`'s cutoff becomes `safe_floor`, not `now - margin`.
6. **Loser-delete + announce interplay.** Verify the producer's create-and-CAS
   path is fine when a slot was just exchanged to null.

## Proof gate

- `test/concurrent.cc` with **margin 0**, many producers, near-horizon scheduling:
  every task fires exactly once (count + XOR), bounded memory, and **zero** ASan
  use-after-free / TSan data races, with the watchdog never firing.
- `test/longevity.cc`: still a plateau.
- A throughput check vs the margin-based `clean` (the announce adds one atomic
  store per `add` and an O(producers) scan per freed subtree).

## Open questions

- Thread-index recycling for producers that come and go (cap `N`, or a free-list).
- Whether the announce scan cost matters at high producer counts (a sorted/
  min-tracked structure could replace the linear scan if so).
- Interaction with `ThreadedScheduler` (tasks handed to a worker pool survive via
  `shared_ptr` regardless; the structure free is independent — confirm).
