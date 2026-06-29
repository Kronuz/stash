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

### The active-clean loss was the wall-clock margin, and walk-based clean closes it

With `clean` running at a finite margin, both `main` and the fixed build lost
~1500–3700 per 3 s run under oversubscription, roughly equally, and the loss did
not shrink as the margin grew (64 ms → 1024 ms, flat and noisy). That ruled out
"margin too small for backlog." TSan then showed **zero data races**, and ASan /
libgmalloc showed **no use-after-free** — so it was not a memory-safety race at
all. It was algorithmic: `clean`'s cutoff is wall-clock `now - margin`, and under
load that reclaims tasks that completed their `add` but went overdue before the
single consumer fired them. The same loss is present in the bare base (b3794af),
so it was never introduced by R1/R2/Steps.

The fix is **walk-based clean**: reclaim up to the walk's live `first_valid`
instead of `now - margin`. With the strand fix, `first_valid` is a safe boundary
(pending leaves pin it, late inserts lower it), so everything below it is walked
and untouched. This is the original "clean as walking", and it needs no SMR.

| producers | margin clean | walk-based clean |
|-----------|--------------|------------------|
| 8         | 26           | **0** |
| 12        | 31           | **0** |
| 13        | 2264         | **0** |
| 14        | 3771         | **0** |
| 16        | ~3000        | **0** |

Memory stays bounded (peak ~50–90 KB, no worse than the margin version). The
residual is the **descent window**: a task that becomes due in the window before
its own insert publishes. It needs insert latency to exceed the task's lead time,
so it cannot occur for any real schedule; at the extreme leading edge (≤8 ms lead)
it is ~1 in millions plain.

> **Correction (see "What the proof gate missed" below).** The "p=14 → 0 lost"
> numbers in this section and the proof gate were produced by a harness that
> grepped output for `FAIL` and so counted a SIGBUS/SIGSEGV (which prints nothing)
> as a pass. At 8–14 producers on this 14-core host the test was in fact
> *crashing*, on **every** version including the bare base — a wheel-wrap aliasing
> use-after-free from the unrealistically small 64 ms span, not the descent window.
> The corrected, crash-aware results are below.

### Sanitizer status

Apple clang 17 on macOS 26.5.1 cannot run the sanitizers (ASan hangs in dyld-cache
init; TSan segfaults at thread create — a 5-line `std::thread` program reproduces
both, so it is the toolchain). **Homebrew LLVM 22** works and is what the proof
gate below uses.

Proof gate (`test/concurrent.cc`, walk-based clean, 14-core host) — **superseded;
the "0 lost" entries below were crash-as-pass artifacts, see "What the proof gate
missed":**

- plain, leading edge (lead 0), p=8/14/16: 0 lost (10/10 repeats at p=14), memory bounded
- ASan, leading edge, p=14 and p=16: no use-after-free, 0 lost
- TSan, lead 16 ms, p=14: 0 data races, 0 lost

The scheduler adopts the same one-line change (`clean` cutoff = `first_valid`,
`pending_floor` reset per walk, R1 margin and R2 keep-out removed); its
`test/test.cc` passes plain and under ASan.

## Sentinel frontier: the COMMIT loop comes out

The strand fix above kept a producer-maintained `atom_ready` (the contiguous
*written* frontier) advanced by a per-insert COMMIT loop. That loop is the whole
cost of the convergent case: every producer landing on the same leaf CASes the
one shared `atom_ready`, and a hole stalls the advance (head-of-line blocking).

The replacement encodes a slot's state in the slot itself, three ways:

- **reserved / fresh** = a `-1` sentinel (chunks are sentinel-initialized before
  they are linked),
- **filled** = a real value pointer,
- **drained** = `null`.

Producers just reserve (`atom_end++`) and fill (`CAS(-1 -> value)`) — no COMMIT
loop, no shared frontier to advance. The consumer's walk reads each slot's state
directly: it drains ready values and skips drained (`null`) ones, **parks
`walk_cur` on the first `-1` hole and never steps it past one** (that is where the
next walk resumes and what keeps the leaf pinned), but it *does* drain ready
values **past** the hole so one slow producer can't stall the rest of the leaf.
`pending = parked-on-a-hole || walk_cur < atom_end` (RMW read of `atom_end`, so
never coherence-stale). `clean` sweeps `[clean_cur, walk_cur)`.

The first attempt *stopped* the walk at the first hole and used `cur < atom_end`
for pending; that collapsed the safety lag and lost ~33% under contention. Draining
past the hole while parking the frontier on it is the correct shape.

### What it buys, and what it costs

Benchmarked (M-series, `-O3`, best of the bench's internal repeats):

- **Convergent (hand-off): ~2.85x throughput at 14 threads** (2.60 → 7.41 M/s)
  and **~3.3x tighter p99** (8.7µs → 2.6µs). The case where stash was *worst* is
  now competitive: above the heap, tied with a locked deque, still behind a
  sharded queue (the right tool for pure hand-off).
- **Spread (the real scheduler pattern): neutral to better** — equal throughput,
  tighter tail at high thread counts (8µs → 5.25µs p99 at 14 threads).

The cost is a **wider descent window**. `atom_ready` (committed frontier) lagged
the reserve frontier by the whole reserve→fill→commit span; the sentinel walk
drains to the reserve frontier, so `first_valid` rides closer to "now" and can
reach an in-flight key sooner. Measured against main (`test/concurrent.cc`, x20):

- supported regime (lead > insert latency: 4 producers; 14 producers / big
  margin; longevity; unit): **0 loss, identical to main**;
- the synthetic knife's-edge regime (8 producers, consumer barely keeping up,
  near-zero lead): both lose to the descent window, the branch **~25% more**
  (19/20 vs 15/20, 2x max loss);
- sanitizers: the same descent-window race/UAF as main, **no new ones**.

The precondition is unchanged from the strand fix: `lead > insert_latency`. Real
schedulers and the logger run leads of milliseconds-to-seconds against
microsecond inserts, so the window never opens; the trade buys the convergent
speed for a residual that only widens in a regime that does not occur in
production. We take it; the precondition stays documented.

> **Correction.** The "branch loses ~25% more (19/20 vs 15/20)" claim above came
> from the same crash-as-pass harness. Those runs were *crashing*, not losing, and
> the crash was wheel-wrap aliasing shared by every version. See the next section
> for the corrected, crash-aware comparison; the sentinel is in fact more correct
> than the base, not less.

## What the proof gate missed: wheel-wrap aliasing, not the descent window

The verification above had two holes, and closing them changed the conclusion.

The first hole was the harness. It decided pass/fail by grepping the test's output
for `RESULT: OK` / `FAIL`. A SIGBUS or SIGSEGV prints nothing, so every crash read
as a silent pass. Re-run counting any non-zero exit as a failure, the picture
flips: at 8 to 14 producers on a 14-core host, `test/concurrent.cc` was *crashing*
most runs, on the sentinel branch, on the strand-fixed main, and on the bare base
alike. (The test now installs a fatal-signal handler that prints
`RESULT: FAIL - crashed (fatal signal)`, so this can never recur.)

The second hole was the test geometry. The wheel spanned 64 ms and wrapped about
16 times a second. Under 8 or more producers the single consumer cannot finish a
clean pass within one revolution, so a physical slot gets reused for a fresh key
while the previous wrap's subtree is still live and uncleaned. The producer walks
into that subtree (spawning a chunk) at the same moment `clean` frees it. ASan
pins it exactly: a producer `compare_exchange` in `Data::get` writing a node that
the consumer's `clean` (`StashSlots::next`) freed. This is **wheel-slot aliasing
from undersizing the span**, a property of any hierarchical timer wheel, and it is
present on every version. It has nothing to do with a task going due mid-insert.

The fix is a sizing invariant, not a code change: **the consumer must complete a
clean pass within one wheel revolution.** Give the wheel a span comfortably above
one clean pass and the crash is gone. With a 4096 ms span (four levels, fine 1 ms
buckets, no wrap during a multi-second run), crash-aware, 14 producers, x12:

| 14 producers, big span | bare base | sentinel |
|------------------------|-----------|----------|
| supported (lead 16 ms) | 7/12 ok, 5 loss, **0 crash** | 11/12 ok, 1 loss, **0 crash** |
| leading edge (lead 0)  | 9/12 ok, 3 loss, **0 crash** | 12/12 ok, 0 loss, **0 crash** |

So once the wheel is sized right there are no crashes, and the sentinel frontier is
not just faster on the convergent case, it is **more correct** under contention
than the base: it sentinel-initializes a chunk before publishing it, so the walk
parks on a reserved slot instead of stepping past an empty hole, and loses far
less. ASan on the big span is clean in the supported regime (fired == added at 14
producers); the only residual is a rare loss at extreme oversubscription (16
threads on 14 cores), where a preempted producer's effective insert latency
occasionally exceeds even a 16 ms lead. That is the documented descent window, now
correctly attributed, and it is bounded loss, never a use-after-free.

### The clean-side grace period, considered and dropped

Before the aliasing was understood, the apparent "descent-window UAF" looked like a
reclamation race to be closed by trailing `clean` behind real time by a grace
margin (`clean` cutoff = `min(first_valid, now - SAFETY)`). Measured, it does not
earn its place. On the correctly sized wheel the sentinel is already clean without
it (12/12 at the leading edge, grace off), and a grace margin is neutral at best
(`SAFETY` 8 ms: same 12/12) and harmful as it grows (16 ms: 11/12; a full `SPAN`:
reintroduces aliasing by holding leaves a whole revolution). The grace margin
fixes a freeing race; the residual is a *firing* miss, which no reclamation policy
can fix. So we do not add it. If a future consumer ever runs at `lead < insert
latency` on purpose, the right answer is to not route those keys through the wheel
(an immediate path or a queue), the way Xapiand's logger already separates
async-now from deferred-future.
