# Closing the strand with `atom_ready` (the self-contained design)

This is an alternative to the announce registry in `CERTAIN_RECLAMATION.md`. It
closes the **strand** — the contention loss where the walker steps over an insert
that is reserved but not yet written — using only the structure's own counters,
with **no external registry, no per-thread state, no `safe_floor` scan**.

The whole idea, in one sentence: a producer reserving a spot already *builds the
path to it and records "pending" in the leaf*, so the consumer can see the
in-flight insert in the tree itself and refuse to walk past it.

## The two facts it rests on

1. **The reserve builds the path.** Descending in `add(key)` lazily creates every
   `StashSlots` child and the leaf *before* the value is written. So the moment a
   slot is reserved, the consumer can already descend to it — it is not invisible.
2. **`atom_end > atom_ready` *is* the "pending here" flag.** Add one leaf counter,
   `atom_ready` — the contiguous *written* frontier. The slot's own pointer is the
   per-slot ready bit (null = reserved-not-written), so no extra per-slot state is
   needed; `atom_ready` just tracks how far the contiguous non-null run reaches.

## Data-structure changes

**`StashValues` (the leaf)** gains one atomic:

```
size_t           walk_cur;    // consumer: next slot to read   (existing)
size_t           clean_cur;   // consumer: GC cursor           (existing)
std::atomic_size_t atom_end;  // producer: next slot to reserve (existing)
std::atomic_size_t atom_ready{0};   // NEW: contiguous written frontier
```

Invariant at rest: `clean_cur <= walk_cur <= atom_ready <= atom_end`. Slots
`[0, atom_ready)` are guaranteed written; `[atom_ready, atom_end)` may contain
holes (reserved, not yet written).

**`StashContext`** gains one field for the walk:

```
unsigned long long pending_floor;   // NEW: lowest key with an in-flight insert
```

reset to `IDLE` (`UINT64_MAX`) at the start of each walk.

## Insert protocol

`add(key)` is reordered into four phases. The crucial change is that the **wheel
bound is published after the reserve but before the fill**, so the walker can
reach the pending leaf while it is still pending.

```
add(ctx, key, value):
  1. RESERVE.  Descend, lazily building the StashSlots path and the leaf
     (CAS-create as today). At the leaf: N = atom_end.fetch_add(1, acq_rel).
     The path now exists and the leaf has a pending slot N (atom_ready <= N).
  2. PUBLISH BOUND.  Widen atom_last_valid_key (and atom_first_valid_key for a
     late key) to cover `key`, as add() does today -- but now *before* the fill.
     The walker can now descend to this leaf.
  3. FILL.  Write the value into slot N (the existing CAS publish of slot[N]).
  4. COMMIT.  Advance atom_ready over the contiguous written prefix:

        r = atom_ready.load(acquire);
        while (r < atom_end.load(acquire)) {
            if (slot[r] is null) break;            // a hole; its producer will advance past it
            if (!atom_ready.compare_exchange_weak(r, r + 1)) continue;  // r updated, retry
            r = r + 1;
        }
```

The COMMIT loop is cooperative and lock-free: whoever fills the slot that *closes*
a gap drags `atom_ready` forward over every already-written slot behind it. Out of
order is handled exactly: if B writes slot 6 while A's slot 5 is null, B's loop
stops at 5; when A writes 5 it advances `atom_ready` over 5, 6, 7 in one sweep.

## Walk protocol

**Leaf (`StashValues::next`, walk/peep).** Read up to `atom_ready`, never
`atom_end`. So the walk only ever sees written slots — no holes to skip. When the
read cursor reaches `atom_ready` and `atom_ready < atom_end`, the leaf is *pending*
(a reserved slot is still unwritten). It records the in-flight key and returns "no
ready value here":

```
limit = (op == clean) ? walk_cur : atom_ready.load(acquire);   // was atom_end
... process [cur, limit) as today ...
if (op != clean && atom_ready.load() < atom_end.load()) {
    ctx.pending_floor = min(ctx.pending_floor, ctx.begin_key);  // this leaf's key
}
```

**Wheel (`StashSlots::next`, walk).** Unchanged for firing — it still scans slots
in key order and returns ready values. The only change is the **`first_valid`
advance is capped at `pending_floor`**:

```
new_first_valid_key = get_dec_base_key(ctx.begin_key);
if (ctx.pending_floor != IDLE)
    new_first_valid_key = min(new_first_valid_key, get_base_key(ctx.pending_floor));
... CAS atom_first_valid_key forward to new_first_valid_key as today ...
```

So the walker keeps firing ready tasks everywhere, but its reclamation low-water
mark `first_valid` never crosses the lowest pending leaf. That is the "hold the
mark but keep walking" rule — the one the naive Step-3 attempt got wrong by
returning "empty" and letting the wheel advance past the whole leaf.

`peep` is read-only: it may pass a pending leaf (it changes no cursor), so it
just skips it and keeps looking ahead. `clean` is unchanged (its range
`[clean_cur, walk_cur)` is already fully walked, so it has no holes).

## Why it is correct

- **No skipped hole.** The leaf walk's upper bound is `atom_ready`, and everything
  below `atom_ready` is written. There is no hole in `[walk_cur, atom_ready)`.
- **No stepped-over leaf.** A leaf with `atom_ready < atom_end` lowers
  `pending_floor` to its key, and `first_valid` is capped there, so the consumer
  re-descends to it next pass. It is never left behind.
- **The walker reaches the pending leaf.** Phase 2 publishes the bound before the
  fill, so `check()` lets the walk descend to the leaf while it is still pending.
  Before phase 2, the bound does not cover `key`, so the walk stops short and
  `first_valid` (bounded by the walk) stays below `key` anyway -- it cannot get
  ahead of an insert it cannot yet see.
- **Ordering / memory model.** Producer: `atom_end.fetch_add` (acq_rel) →
  bound CAS (release) → `slot[N]` publish (release) → `atom_ready` CAS (acq_rel).
  Consumer: `atom_ready.load` (acquire) gates the leaf read; reading a slot
  `< atom_ready` therefore happens-after that slot's release-write. The
  `pending_floor` comparison reads `atom_end`/`atom_ready` with acquire.

## Edge cases

- **Walk consumes vs producer COMMIT.** The walk consumes (`exchange(nullptr)`)
  slots in `[walk_cur, atom_ready)`. The COMMIT loop only inspects slots
  `>= atom_ready`. The two ranges are disjoint (`walk_cur <= atom_ready`), so a
  consumed null is never mistaken for a hole.
- **`clean` drop (the A reclamation).** `clean` frees a subtree only below
  `first_valid`. With the cap, `first_valid <= pending_floor`, so a leaf with any
  in-flight insert is never freed. A freed leaf has `atom_ready == atom_end` and
  is fully consumed.
- **Leaf destructor.** Unchanged; only runs on a fully-past, non-pending leaf.

## Liveness

If a producer is descheduled between RESERVE and COMMIT, its leaf stays pending,
so `first_valid` is pinned at that key and reclamation of that point waits for the
producer to resume. Nothing is lost or double-fired; only the low-water mark
stalls, bounded by that one producer's scheduling delay. For a single-consumer
scheduler this is benign (the consumer has nothing better to do than wait for the
imminent write). The walker still fires every *ready* task above the pinned point.

## What it closes, and what it does not

- **Closes: the strand** (leaf holes and the wheel stepping past a pending leaf) —
  the contention loss this whole thread is about.
- **Also helps R1 from the leaf inward:** an insert that has reached its leaf pins
  `first_valid`, so `clean` cannot free under a reserved-and-filling producer
  *without* a time margin for that window.
- **Does not, by itself, cover the descent window of R1:** between entering
  `add()` and the leaf RESERVE, a producer holds intermediate pointers but has set
  no `atom_end`, so it is invisible. A `clean` that frees a past subtree sharing a
  physical slot (modulus) with that in-flight path could still race it. The
  existing **R1 clean margin** remains the backstop for that brief descent window;
  this design shrinks R1's exposure to it but does not remove the margin.
- **R2 (near-horizon aliasing)** is orthogonal and already handled by
  `horizon_margin`.

So the honest framing: this makes the **strand** exact and self-contained, and
narrows R1 to just the pre-reserve descent — it composes with the R1 margin and R2
rather than replacing them.

## Implementation steps (incremental, harness-gated)

Each step keeps `test/test.cc`, `test/longevity.cc`, and `test/concurrent.cc`
(multi-producer, ASan + TSan) green.

1. Add `atom_ready` and the COMMIT loop to `StashValues::put`; leaf walk reads
   `atom_ready`. (Closes the leaf hole; expect the loss to drop, not vanish.)
2. Reorder `add()`: split `put` so RESERVE+bound precede FILL+COMMIT.
3. Add `ctx.pending_floor`; leaf lowers it; wheel caps `first_valid`. (Closes the
   wheel half.)
4. Drop the `concurrent.cc` margin toward the add-latency floor and confirm
   accounting stays exact under ASan/TSan.

## Proof gate

`test/concurrent.cc`, many producers, near the leading edge: every task fires
exactly once (count + XOR), bounded memory, zero ASan use-after-free, zero TSan
races, watchdog never fires — with the *clean margin still in place* for the R1
descent window. Plus a throughput check vs `main`: the cost is one extra atomic
counter, the COMMIT loop (amortized O(1) per insert), and an `atom_ready` load per
leaf walk.

## Risks / open questions

- The "hold the mark, keep firing" cap is exactly what broke in Step 3; it needs
  careful implementation and a TSan pass, not a quick patch.
- COMMIT-loop contention when many producers hammer one leaf (the CAS on
  `atom_ready`). Likely fine (amortized one advance per insert), but measure.
- `peep` passing a pending leaf must not perturb the cursors — verify.
- Whether, with care, the descent window can also be linearized so the R1 margin
  could finally drop — a follow-on, not part of this addition.

## Results (implemented: Step 1 = commit `8922441`, Steps 2+3 = `744b82f`)

Measured on a 14-physical-core host with `test/concurrent.cc`. Loss = added −
fired (count and XOR both checked). Each producer is one thread; the consumer and
a watchdog add two more, so `producers + 2` is the thread count against 14 cores.

### Step 1 alone regresses under oversubscription

`atom_ready` closes the strand perfectly while threads ≤ cores, but once
oversubscribed it loses *more* than `main`, because a producer preempted between
RESERVE and FILL pins the contiguous frontier and hides every slot filled behind
its hole (head-of-line block); with no `pending_floor` cap the wheel advances
past that leaf and `clean` drops the whole hidden tail.

| producers | threads | main | Step 1 only |
|-----------|---------|------|-------------|
| 10–11     | 12–13   | 53–62 | **0** |
| 12        | 14      | 47    | 56 |
| 13        | 15      | 232   | **3130** |
| 14        | 16      | 2295  | 1466 |

### Steps 2+3: structurally lossless

With `clean`'s reclamation isolated (margin larger than the run, so any loss is a
structural race rather than a reclamation drop), Steps 2+3 lose nothing at any
producer count, oversubscribed or not — 4/4 repeats at 13/14/16 producers all
read 0. `main` keeps losing the strand everywhere.

| producers | main | Steps 1+2+3 |
|-----------|------|-------------|
| 10        | 85   | **0** |
| 12        | 74   | **0** |
| 13        | 59   | **0** |
| 14        | 44   | **0** |
| 16        | 50   | **0** |

The leaf strand is closed (Step 1 at threads ≤ cores) and the head-of-line tail
is recovered rather than dropped (Steps 2+3 under oversubscription). `test.cc`,
`longevity.cc`, and the leak→plateau demo stay green.

### What is *not* closed: the reclamation race with active clean

Under oversubscription **with `clean` running at a finite margin**, both `main`
and the fixed build still lose ~1500–3700 per 3 s run, roughly equally, and the
loss does not shrink as the margin grows (64 ms → 1024 ms is flat and noisy).
That is not the strand — `main` has no `atom_ready` yet loses the same. It is a
reclamation race: `clean` frees a subtree while a *preempted* thread still holds
a reference into it. A wall-clock margin cannot close this with certainty under
arbitrary preemption (the whole reason certain reclamation needs SMR, not a
timer). It is the R1/R2 boundary, documented as out of scope for this addition.

### Sanitizer status (host toolchain limitation)

ASan and TSan could not be run on this host: Apple clang 17 on macOS 26.5.1.
ASan hangs during runtime init (`__asan::InitializeShadowMemory` spins iterating
the dyld shared cache); TSan segfaults at thread creation — a 5-line
`std::thread` program reproduces both, so it is the toolchain, not this code.
The ASan/TSan rows of the proof gate must run on Linux/CI or a newer LLVM.

As a substitute, `libgmalloc` (guard-page allocator, unmaps on free so any
use-after-free faults immediately) ran the oversubscribed, clean-on config across
~174k inserts with **no guard-page fault**. Combined with the no-UAF accounting
above, this indicates the residual loss is dropped/detached subtrees (accounting)
rather than writes into freed live memory — though it is weaker evidence than a
clean TSan/ASan pass and does not replace one.
