# Code review: fix/descent-window (the in-flight announce)

Branch `fix/descent-window`, tip `9e2ebd8` (commits `bd8c2c6`, `d7dc92b`, `9e2ebd8`) from
main `2908b48`, matching `origin/fix/descent-window`. Reviewed from the detached worktree
`~/code/Kronuz/stash-wt/review-descent` on macOS x86_64 (i9-9980HK, 16 logical CPUs),
Homebrew LLVM for sanitizers. CI run 39364572180 is green per the lead; not re-checked.

## Verdict: APPROVE WITH CONDITIONS

The handshake is sound and the fix works. Under the developer's starvation method, base loses
tasks and the fix loses none. An exhaustive model of the algorithm finds no violation, and every
mutant that removes a safety mechanism does. Nothing blocks landing. The conditions are a missing regression test for the
walk-start rule, a consumer contract that became load-bearing without being written down, a
cache-line layout slip, and doc corrections.

## What was exercised

| Check | Result | Evidence |
|---|---|---|
| CTest, asserts live (`-O2 -g`, CI's flags) | 2/2 pass; `stash_concurrent` runs `4 2.0 32 0 0`, lead 0 | `CMakeLists.txt:23`; `.scratch/build` |
| Starved A/B, interleaved, 48 normal-priority busy loops, test at nice 19, 4 producers, 2 s, lead 0, 30 runs each | base: 4 runs failed, 5 lost of 79,341,750; fix: 0 failed, 0 lost of 77,064,288 | `.scratch/logs/starve-ab.log`, `.scratch/probes/starve_ab.sh` |
| `test/stress.sh BIN 30 4 2.0 0 48` (its own nice'd-hog mode) | base: 10 runs failed, 12 lost of 80,065,848; fix: 0 failed, 0 lost of 76,535,041 | `.scratch/logs/stress-sh-{base,fix}-hogs48.log` |
| TSan `concurrent` 4 producers, lead 0, 5 runs | 0 warnings, 0 lost (~4.9M tasks) | `.scratch/logs/concurrent-tsan.log` |
| ASan+UBSan `concurrent` 16 producers, lead 0, 5 runs | 0 errors, 0 lost (~40.9M tasks) | `.scratch/logs/concurrent-asan.log` |
| ASan+UBSan and TSan `test.cc`; ASan probes | clean | `.scratch/logs/unit-sanitizers.log` |
| Exhaustive model of the handshake (SC interleavings, 2 producers, 1 consumer, walks plus clean), 1 and 2 announce slots, refresh period 1 to 3, both consumer styles | fix: no violation (1.5M to 5.5M states per configuration); every mutant except `no_pinned` violates | `.scratch/model/handshake_model.py`, `results*.txt` |
| Mutants vs `test.cc` and the probes | see DW2 | `.scratch/mutants/*`, `.scratch/bin/{test,probe}_*` |
| Interleaved throughput A/B, main vs fix, 4 rounds, strict hold | -0.4% to -2.4% spread, -1% to -4% convergent | "Performance"; `.scratch/logs/bench-ab.log` |
| Liveness: stalled producer, many-thread overflow on the scheduler's wheel | DW5, DW6 | `.scratch/logs/liveness*.log` |

The developer's starvation logs are consistent with these runs: base lost 3 of 74.5M in 30 runs,
and the fix lost 0 of 255M in 100 runs (`stash-wt/descent-window/build/logs/`). Pooled, the fix
failed 0 of 160 starved runs, against a base failure rate of 17 in 90 runs. If the fix had not
changed the failure rate, a clean run of that length would happen with probability about
(73/90)^160 ≈ 3e-15.

## The proof, checked (question 1)

**Store-load handshake.** The walk stores the floor (seq_cst, `stash.h:229`), then loads each
slot (seq_cst, `:233`) and the overflow counter (seq_cst, `:238`). The producer CASes its slot
(seq_cst on success, `:177`) or runs `fetch_add` on overflow (seq_cst, `:186`), then loads the
floor (seq_cst, `:189`). Suppose the walk's slot load reads a value modification-ordered before
the producer's CAS. That load is coherence-ordered before the CAS, so it precedes the CAS in S.
Sequenced-before between seq_cst operations gives floor-store <S slot-load <S CAS <S floor-load.
A seq_cst load cannot then read a floor older than the walk's store. So either the walk sees the
announce or the producer sees the floor. Each of the four accesses must be seq_cst, and they all
are. The same argument covers the overflow counter.

The other orders hold up:

1. The relaxed pre-claim floor load (`:167`) only chooses the announced value. By read-read
   coherence, the later seq_cst load (`:189`) reads the same floor or a newer one. So the
   announced key is at most the filed key, which errs low (safe).
2. Retraction uses a release store (`:196`) or a release `fetch_sub` (`:198`). A walk's seq_cst
   load that reads IDLE, or a later claimant's CAS in the same release sequence, synchronizes
   with it. So a walk that misses a finished producer sees its value and its `last_valid` raise.
3. The CAS failure order (relaxed) never matters, because a failed claim publishes nothing.

The `weak_mo` mutant (floor store release, loads acquire) passes every test, TSan and all
x86 stress runs. No tool in this setup can detect a weakening. The proof above and the
AGENTS.md invariant ("weakening either side ... reopens the descent window") are the only
guards. See "Not run".

**Algorithm.** The model has the per-step granularity of the code: a separate step for each
slot read, for each producer's claim, floor read, reserve, last_valid raise, fill and release,
and for each walk visit and ret_next publish, plus clean freeing below first_valid. It checks
three properties: no UAF (a producer touches a freed leaf), no FV (first_valid passes a filing
key in flight), and no LOSS (clean frees an undrained entry, or an entry is still undrained after
the final drain). Results:

| Mutant | cont (begin_key once per walk) | sched (begin_key = first_valid every call) |
|---|---|---|
| fix | no violation | no violation |
| `every_call` (revert d7dc92b) | LOSS | no violation |
| `no_cap` | FV | FV |
| `no_floor_raise` | UAF | UAF |
| `no_overflow_cap` | FV with 1 slot, none with 2 | same |
| `no_pinned` | no violation | no violation (liveness only) |
| `no_announce` | FV | FV |

**Multi-call walks.** The rule "refresh only on a call that starts at first_valid"
(`stash.h:217-219`) is complete. Within a walk, first_valid never rises above
`dec_base(begin_key) < begin_key`. So `begin_key <= first_valid` holds only before the walk has
passed anything, and a snapshot there covers every later visit. The model confirms that the
rule is needed only for continuation walks. Kronuz/scheduler's `walk()` resets `begin_key` to
first_valid on every call (`scheduler.h:167-174`) and is safe either way.

**`clean` use-after-free.** Fixed. While a producer is in flight, first_valid stays at or below
its filing key. `clean` frees only top-level slots whose whole window is at or below its
`end_key`, and the consumer loads that bound from first_valid (`stash.h:559`). The producer's
path lies inside slots that contain its key, so `clean` never reaches it. The model's UAF check
fires only on the `no_floor_raise` mutant.

## Findings

### DW1: The walk-start rule (d7dc92b) has no deterministic test. Condition.

`test/test.cc:195-206` (`drain`) resets `begin_key` to first_valid on every call, the
scheduler's style. Reverting d7dc92b therefore leaves `test.cc` passing (mutant `every_call`:
rc 0). Only a starved stress run catches the revert: the developer's run lost 2 in 20. Probe P1
(`.scratch/probes/probe_descent.cc`, `probe_continuation_walk`) catches it deterministically.
Call 1 snapshots an announce at 3 and returns the value at 5. The producer then lands 3 and
retracts, and call 2 continues from 5. The fix fires 33 on the next walk. `every_call` strands
it (P1 FAIL), and so does `no_cap`. Add P1, or an equivalent, to `test.cc`.

### DW2: The new `test.cc` checks are mutant-sensitive, but they do not regress the original bug. Note.

Against main's `stash.h`, `test.cc` does not compile, because it uses `InFlight`. Against the
mutants:

- `no_cap` fails at `test.cc:220`.
- `no_floor_raise` fails at `:238`.
- `no_overflow_cap` fails at `:256`.

So each of the three checks pins one mechanism. The original loss, a concurrent race, is covered
only by the lead-0 `stash_concurrent` in CTest and by `test/stress.sh`. Unstarved, CTest's 4
producers for 2 s rarely trip on base. The run that cost 1 in 3.1M was the nightly
macos-x86_64 job. This is acceptable, but the commit message's "test.cc covers the pin" should
not be read as a regression test of the race.

### DW3: Starting each walk at `begin_key = first_valid` is now load-bearing for liveness and is undocumented. Condition.

`refresh_first_valid_cap` returns early when `begin_key > atom_first_valid_key`
(`stash.h:217-219`). A consumer that carries `begin_key` over from its previous walk therefore
never refreshes. `first_valid_cap` stays at the constructor's key, first_valid never advances,
`clean` reclaims nothing, and `add` throws once keys pass one span. Probe P7
(`.scratch/probes/probe_no_reset.cc`, Div 1, Mod 8, 40 one-key walks):

```diff
- base: fired=40 add-throws=0  first_valid=40
+ fix:  fired=8  add-throws=32 first_valid=1
```

Every in-tree consumer follows the protocol: `scheduler.h:170`, `test/concurrent.cc`,
`test/longevity.cc`, the benches, and `examples/demo.cc:41-52`, which spells it out. So nothing
breaks today. But the protocol now decides liveness, and neither `docs/API.md` nor AGENTS.md
states it. Write it into API.md's walk contract and into AGENTS.md's invariants. A debug-mode
assert would also catch it: if a walk op's `begin_key` never equals first_valid within N calls,
fail.

### DW4: Consumer-only fields share `atom_insert_floor`'s cache line. Condition (cheap).

`stash.h:125-131`: `atom_insert_floor` is `alignas(64)`, but `insert_floor`,
`first_valid_cap` and `depth` follow it on the same line. Probe P4 prints line 66 for all four.
`depth` is written twice per `StashSlots` level on every `next()` call (`stash.h:518-522`),
which is 8 writes per call on the scheduler's 4-level wheel. Every `add` loads that line twice
(`:167`, `:189`), so each consumer call can invalidate every producer's copy of the floor. Move
the consumer-only fields above `announce`, or pad `atom_insert_floor` to a full line. A
pre-existing case is the same in kind: `atom_first_valid_key` and `atom_last_valid_key` share
line 0 with `op`, `begin_key`, `end_key` and `pending_floor`. Not measured separately; see the
performance section.

### DW5: A stalled producer pins first_valid across the whole of `add`. Past one span, every `add` throws. Condition (docs).

The pin was already there between reserve and fill (`pending_floor`). It now covers the whole
insert, so it is wider, but it is bounded by how long the OS preempts the producer. Its effects:

1. `clean` frees nothing at or above the pin, so memory grows with the stall.
2. Kronuz/scheduler's `walk()` restarts from first_valid on every call, so each fired task
   re-walks the pinned range, one leaf per 1 ms of stall. Probe `probe_liveness stall`
   (the scheduler's wheel, 4 producers, one `InFlight` held mid-add) measured the cost of a
   `next()` call:

   | Stall | Cost of a `next()` call during the stall |
   |---|---|
   | none | 200 ns |
   | 100 ms | 847 ns |
   | 1 s | 4.3 µs |
   | 3 s | 8.7 µs |

   That is roughly 3 ns per pinned millisecond. No task was lost and no `add` threw
   (`.scratch/logs/liveness.log`).
3. Once the floor passes `base(first_valid) + span - horizon_margin`, the floor raise lifts
   even an overdue key past the horizon, and `add` throws (`stash.h:663-674`). Probe P3: a
   stall pins first_valid at 1 with the floor at 19 on a span-8 wheel, and an overdue `add(3)`
   throws `out_of_range`. Kronuz/scheduler's `add` catches the exception and drops the task
   silently (`scheduler.h:195-210`).

Nothing in `add` blocks, so a producer blocked on I/O is never inside it. With Xapiand's
trace header, `L_STASH` is a no-op (`src/xapiand_stash_trace.h:16`, `CMakeLists.txt:974`).
The announce spans only the floor read, the horizon check, the descent, the chunk
allocations, the value copy and the fill. Only three things can hold it for long:

1. Preemption.
2. `malloc` contention.
3. A page fault on a fresh chunk. The scheduler's top level allocates a 38 KB chunk of 4,800
   atomics.

On the scheduler's 24 h wheel, effect 3 is unreachable in practice. On `test/concurrent.cc`'s
4 s wheel it takes a 4 s stall. AGENTS.md:125 names the pin on `clean` and the horizon but not
the consequence that every `add` throws (overdue ones included) and is dropped by the
scheduler. Add that consequence to the AGENTS.md trap and to ARCHITECTURE's limitations.

### DW6: Sustained overflow freezes first_valid. For the architect.

While `atom_announce_overflow` is nonzero at a refresh, the cap becomes wherever first_valid
stands (`stash.h:238-243`). The cap is then below the floor (pinned), so each later walk start
re-checks. If more than 64 producers are in `add` at every snapshot, first_valid never moves,
which ends as in DW5 item 3.

**Against Xapiand's workload.** The workload is deferred logging from hundreds to thousands of
request threads, most of them blocked at any instant. Blocked threads are never inside `add`
(DW5), so the number of producers in `add` at once is about the number of runnable threads
times the fraction of their CPU time spent in `add`. An `add` costs about 0.5 to 1 µs (see the
performance section). A request that logs a few lines spends well under 1% of its CPU time in
`add`. Overflow therefore needs thousands of runnable threads, or threads that do little except
log. Probe `probe_liveness overflow N SECONDS PAUSE_US` (the scheduler's wheel), with the
sampled fraction of consumer walks that saw overflow nonzero:

| Producers, shape | Base | Fix |
|---|---|---|
| 64, 256, 512; 1 µs pause | n/a | 0% |
| 1,000; 1 ms pause between adds (mostly blocked) | keeps up | keeps up, 0% |
| 2,000; 1 ms pause | consumer starved | consumer starved, 0% |
| 1,000; no pause (all runnable, adding back to back) | consumer starved: 59K of 16.8M fired | consumer starved: 34K of 17.2M fired, 100% of 6 sampled walks |

In the mostly-blocked runs, both builds keep up with similar lag: max 136 ms on base and
160 ms on the fix, mean 1.7 ms on both. Overflow appears only in the adversarial shape, where
1,000 threads do nothing but `add`. There main's consumer is already starved, so the freeze
changes nothing that matters. Each freeze also lasts only until a refresh sees the counter at
zero. It is not sticky.

My verdict for Xapiand: 64 slots with the overflow counter is adequate, and overflow is not
expected to hold first_valid in the real workload. The cap of 64 is still undocumented: the
code names its mechanism (the overflow counter), but neither the docs nor the commit say why
64 is enough. Record the sizing argument above, or one like it, in ARCHITECTURE.md.

### DW7: Overdue keys fire in insertion order, not key order. Informational; documented.

Probe P2: keys 2 and 1, added in that order after a walk published floor 7, are both filed at
the floor and fire 22 then 11. ARCHITECTURE.md ("Inserts in flight", last paragraph) states
this. Kronuz/scheduler carries `wakeup_time` in the task and does not depend on the order among
overdue tasks. Every consumer uses `steady_clock`, so a floor ahead of a backward-jumping clock
cannot delay a task.

### DW8: `put` is unsafe concurrently, and only docs enforce it. Note.

`put` stays public (`stash.h:636`), and the inner levels call it recursively. No caller in
Xapiand's own code, Kronuz/scheduler, Detent's sources, or stash's benches and examples calls
`put` on a shared context. The only call is `test/test.cc:221`, under an `InFlight`. The
AGENTS.md:122 trap and the ARCHITECTURE limitation are enough for now. A later API pass could
make `put` protected and befriend the level types.

### DW9: StashContext grows from 56 to 4,288 bytes, 64-byte aligned. Acceptable; one waste.

P4 and `probe_layout_main` report 56 bytes on main and 4,288 on the branch.

- **Xapiand:** 8 schedulers (the logger, plus the committer, fsyncher and trigger_replication
  debouncers and the db, schema, settings and primary updaters; `src/manager.h:329`,
  `src/xapiand_fsyncher.h:30`, `src/database/handler.h:331`, `src/server/discovery.h:192-216`),
  2 contexts each, about 67 KB. They are process singletons, not per index, so the
  many-indexes workload is unaffected.
- **Detent:** the logger's scheduler only, about 8.4 KB. The library is pinned at `2908b48` in
  `cmake/dependencies.json`.
- **Allocation:** `std::make_unique` handles the over-alignment through C++17 aligned new.

The waste: `SchedulerQueue::cctx`, the clean context, carries a 4.2 KB announce array it never
uses. Moving the announce state out of `StashContext` into a producer-side object would halve
the cost. Not a condition.

### DW10: Doc corrections. Condition.

1. `docs/API.md:11-12` says "`add` widens them" of `atom_first_valid_key` and
   `atom_last_valid_key`. Now `add` raises only `atom_last_valid_key`.
2. `ARCHITECTURE.md:313` ("one store and one pass over the slots per millisecond ... not per
   task") holds only while no snapshot is pinned. While `first_valid_cap < insert_floor`
   (`stash.h:223`), every call that starts at first_valid re-reads all 64 slot lines. Under
   Kronuz/scheduler that is every `walk()`, so once per task. Lines 314-315 half-say this.
   State it plainly.
3. Pre-existing: `stash.h:673` throws `"stash overlow"`, while API.md and AGENTS.md say
   `"stash overflow"`.

### DW11: A throwing value constructor leaves a permanent reserved hole. Pre-existing, out of scope.

Probe P5: when `_Tp(args...)` throws after the leaf slot is reserved, the announce is retracted
correctly (RAII, PASS). The sentinel stays forever, though, and pins first_valid at that key
through `pending_floor` (first_valid stays at 2). This is the same on main and not introduced
here. Recorded for the architect.

## Per-thread announce ownership (the variant was dropped)

The coordinator dropped the per-thread slot variant, and `9e2ebd8` stays the candidate. These
hazards are recorded in case the variant returns. Today's CAS claim has none of them.

1. **Context reuse (ABA).** If ownership is keyed by the context's address, a context
   reallocated at the same address inherits stale ownership. Two threads then share one slot,
   and one thread's retraction erases the other's announce mid-descent.
2. **Thread exit.** Slots owned by exited threads must return to the pool, or the slots run
   out and every `add` takes the overflow path.
3. **Destruction order.** A `thread_local` destructor can release a slot in a context that
   has already been destroyed.
4. **Reentrant `add` on the same context from the same thread.** A value constructor, an
   allocator, a trace hook routed to the logger, or a signal handler can re-enter `add`. A
   reentrant `add` would overwrite and then retract the outer announce while the outer insert
   is still in flight.
5. **The store must stay seq_cst.** On x86 that is `xchg`, which costs about the same as
   `lock cmpxchg`.
6. **fork().** A child inherits ownership records for threads that do not exist in it.

## Performance

The A/B used the developer's `stash_only` benchmark (`bench/stash_only.cc`, Xapiand's 4-level
wheel, one consumer draining, 2M inserts, best of 3 per cell). It ran as 4 interleaved rounds
of main `2908b48` against `9e2ebd8`, alternating order, under a strict hold on an idle host
(`.scratch/bench/ab.sh`, `.scratch/logs/bench-ab.log`). The table gives the median throughput
of the 4 rounds in million inserts per second, with the range in brackets:

| Threads | Spread base | Spread fix | Δ | Convergent base | Convergent fix | Δ |
|---|---|---|---|---|---|---|
| 1 | 1.02 [1.01-1.02] | 1.00 [0.99-1.01] | -1.5% | 1.94 [1.82-1.98] | 1.87 [1.84-1.92] | -3.9% |
| 2 | 1.47 [1.44-1.47] | 1.44 [1.43-1.45] | -2.4% | 2.56 [2.33-2.60] | 2.47 [2.45-2.49] | -3.7% |
| 4 | 2.08 [2.07-2.09] | 2.04 [2.04-2.05] | -1.7% | 3.02 [2.97-3.82] | 3.46 [3.35-3.54] | noise |
| 8 | 2.87 [2.84-2.88] | 2.82 [2.77-2.83] | -1.6% | 5.88 [5.72-5.91] | 5.79 [5.77-5.82] | -1.5% |
| 14 | 3.92 [3.90-3.97] | 3.91 [3.88-3.96] | -0.4% | 7.61 [7.29-7.68] | 7.53 [7.45-7.57] | -1.1% |

The fix costs 1.5-2.4% of insert throughput on spread keys at 1 to 8 threads, where the ranges
do not overlap, and nothing measurable at 14 threads. On convergent keys it costs 1-4%. The
4-thread convergent cell is bimodal on base (2.97 to 3.82), so read nothing into its +14%. The
p99 insert latency is unchanged within ±2% in every cell. At one thread, 1.02 to 1.00 Mops is
about 20 ns more per insert. That is the locked CAS on the announce line plus the floor load.
For deferred logging, where inserts are a sliver of request CPU, the cost is negligible. This
agrees with the developer's 1-5% and 1-3%.

DW4's false sharing is included in these numbers and was not isolated. The consumer side, a
64-line slot scan at most once per finest slot, or per task while pinned (DW10 item 2), was
not measured separately.

## Blocking items

None.

## Conditions

1. DW1: add a deterministic continuation-walk test (P1) to `test/test.cc`.
2. DW3: document "start every walk at `begin_key = atom_first_valid_key`" in API.md's walk
   contract and in AGENTS.md's invariants, with the liveness consequence.
3. DW4: move `insert_floor`, `first_valid_cap` and `depth` off `atom_insert_floor`'s cache
   line.
4. DW5 and DW10: the doc fixes (the stall-past-span consequence, API.md's stale "`add` widens",
   ARCHITECTURE's per-millisecond cost claim).
5. DW6: record in ARCHITECTURE.md why 64 slots with the overflow counter suffice for
   Xapiand's workload (the argument and numbers in DW6).

## Probes, harnesses and mutants (kept in `.scratch/`)

- `.scratch/model/handshake_model.py`: the exhaustive model (`python3 handshake_model.py
  MUTANT STYLE`; `PERIOD=N` env); results in `results.txt` and `results-period.txt`.
- `.scratch/probes/probe_descent.cc`: P1 (continuation walk), P2 (overdue order), P3 (stall
  vs horizon, slot retraction on throw), P4 (layout), P5 (throwing value constructor), P6
  (overflow insert lands).
- `.scratch/probes/probe_no_reset.cc`: P7 (carried `begin_key`).
- `.scratch/probes/probe_layout_main.cc`: `sizeof(StashContext)` for main and the branch.
- `.scratch/probes/probe_liveness.cc`: overflow lag (`overflow N SECONDS PAUSE_US`) and
  stall cost (`stall MS SECONDS`); `-DBASE` builds it against main.
- `.scratch/probes/starve_ab.sh`: interleaved starved base/fix runs.
- `.scratch/mutants/{base,every_call,no_cap,no_floor_raise,no_overflow_cap,weak_mo}/stash.h`.
- `.scratch/bench/stash_only.cc` (copied from the developer's `build/bench/`), `ab.sh`.
- Binaries in `.scratch/bin/`, logs in `.scratch/logs/`, CTest build in `.scratch/build/`.

## Not run

- A weak-memory model checker (GenMC, CDSChecker, Relacy) on the C++ itself. The model is
  sequentially consistent, so memory orders rest on the S-order argument above. The
  `weak_mo` mutant is undetected by every tool run here.
- Linux and arm64. Nothing ran pinned to 2 cores with `taskset`, so the 200-350 of 300k
  figure was not reproduced; all runs were on macOS x86_64.
- TSan under starvation, and longer TSan runs: 5 × 2 s only.
- `test/longevity.cc` (manual; not under ASan by design).
- The per-thread variant (dropped by the coordinator; not reviewed).
- A Linux run of the many-thread overflow probe; the macOS consumer starves at 2,000 threads
  in both builds, so those rows show no difference between base and the fix.
- Building Xapiand, Kronuz/scheduler or Detent against the branch.
- CI: run 39364572180 not inspected per platform.
