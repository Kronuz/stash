# Path B — implementation log

A sequential record of building **certain reclamation**, one step per commit. The
design (the *what* and *why*) lives in [CERTAIN_RECLAMATION.md](CERTAIN_RECLAMATION.md);
this log is the *how*, in order, with the verification result for each step.

The goal: free subtrees correctly under arbitrary concurrency with **no wall-clock
margin**, by having producers announce the key they are inserting and the consumer
free only what is provably unreachable.

### Gates (every step must keep these green)

- `test/test.cc` — smoke: key-order walk, underflow/overflow guards.
- `test/longevity.cc` — leak → plateau: proves reclamation actually reclaims.
- `test/concurrent.cc` — producer/consumer contention. Accounting (every task
  fires exactly once, count + XOR) plus ASan / TSan. A watchdog turns any hang
  into a reported FAIL. Args: `producers seconds margin_ms throttle_ns [reclaim]`.

---

## Step 0 — design + branch

Design written (`CERTAIN_RECLAMATION.md`): a single mechanism — producers announce
their in-flight key — fixes all three known races (boundary use-after-free,
modulus reacquisition, bounds-ordering strand) and removes the time margin.
Work happens on branch `wip/certain-reclamation`.

## Step 1 — announce registry, inert  (`aa9a1a3`)

**What.** `reclaim.h`: a per-thread announce array. Each producer publishes the key
it is inserting in *its own* slot (one stable index per thread → contention-free);
the consumer can read the minimum in-flight key, `safe_floor()`. `StashContext`
gains a null-default `reclaim` pointer; the top-level `add()` wraps the insert in
an RAII `InFlight` announce (covers descent + bounds update, exception-safe).

**Why.** Put the *knowledge* (lowest in-flight key) in place before anything uses
it, so the plumbing can be proven harmless on its own.

**Verify.** Behavioral no-op — nothing reads `safe_floor()` yet, and `reclaim` is
null in every existing harness. smoke passes; single-producer concurrent is 0 loss;
longevity still plateaus to 80 B. Identical to pre-B.

## Step 2 — walk clamp (partial; reveals a second strand)  (`<this commit>`)

**What.** In the walk, clamp the `first_valid` advance at `safe_floor()` — the
walker's low-water mark may never pass a key a producer is still inserting. The
concurrent harness now attaches a `reclaim` Domain (5th arg, default on).

**Why.** Close the bounds-ordering strand at the wheel level — `first_valid`
stepping over a late insert.

**Verify / finding.** It helps but does **not** close the loss. 4 producers,
margin 16: reclaim OFF lost 104/197k, reclaim ON lost 75/197k; single-producer
stays 0; smoke unchanged. The residual is a **second strand, at the leaf**:
`StashValues::put` reserves a slot with `atom_end++` and fills it a few
instructions later; the walk can read `atom_end`, scan the still-empty slot, skip
the "hole", and advance `walk_cur` past it — stranding the task the producer is
about to write. The wheel clamp can't see this (it is key-based; the hole is
position-based). This is the *leaf* half of the "linearized insert/walk" the
design calls for. → Step 3.

**Note on cost.** B is turning out to have more than one linearization point. Each
is tractable, but the count is a thing to watch: if the fixes keep multiplying
without converging, that is the signal to stop (see the project's "drop it if it
isn't a jewel" bar).

## Step 3 — leaf linearization: a structural wall (reverted)

**Attempt.** Make the walk stop at an unfilled leaf hole instead of skipping it.

**Result.** Worse, not better: 4 producers / margin 16 jumped to ~11.7k lost, and
single-producer (always 0) broke. Reverted.

**Why it can't be a small fix.** Returning "false" from the leaf on a hole tells
the *parent* (the wheel walk) "this leaf is empty", so the wheel advances past the
whole leaf -- stranding everything in it. Not returning means the walk must *wait*
at the hole. And because producers reserve with `atom_end++` and fill **out of
order**, there is no single "committed up to N" counter that is safe; the only
correct construction is a **per-slot ready flag** (a Vyukov-style sequenced MPMC
buffer): each slot publishes when it is written, and the walk scans contiguously,
waiting at the first not-ready slot. That replaces the elegant append-only leaf
with a much heavier structure.

**Where this leaves B.** Full certainty needs three things, not one:
1. The SMR free (announce + hazard-scan + retire) -- designed, not built.
2. The wheel-level walk clamp -- built (Step 2), cheap, partial.
3. A sequenced-MPMC leaf -- a real redesign of the core leaf.

Items 1 and 3 are each substantial, and together they bury the thing that made
`stash` a jewel (a simple sparse lock-free wheel) under reclamation and
linearization machinery. This is the "complex trash" line the project set out not
to cross. Recommendation recorded for the keep/drop decision: **lean drop** --
keep A (shipped, correct in its documented envelope), preserve this branch as the
honest record of what full certainty would cost and why we stopped.
