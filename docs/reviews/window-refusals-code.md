# Code review: the window handshake, fail-fast, and refusal handling (four branches)

Reviewer: code reviewer (fresh context), 2026-10-20. Sensitive tier (concurrency, durability).

| Repository | Branch | Reviewed commit | Review worktree |
|---|---|---|---|
| Kronuz/stash | `perf/window-epoch` | a080a1e | `~/code/Kronuz/review-wt/stash` |
| Kronuz/scheduler | `perf/lazy-rearm` | 5319dc5 | `~/code/Kronuz/review-wt/scheduler` |
| Kronuz/logger | `fix/refused-add` | 2c2a264 | `~/code/Kronuz/review-wt/logger` |
| Xapiand | `fix/debounce-refusals` | 903ad2350 (local only) | `~/code/Kronuz/review-wt/xapiand` |

All four review worktrees are detached at the reviewed commits. The three library branches
match `origin` (no unpushed commits), and GitHub CI is green on each reviewed head. Xapiand's
branch is local only.

## Verdict: APPROVE WITH CONDITIONS

No blocking items. The window handshake holds under the model (reproduced, plus two new
memory-order mutants and a new interleaving), under TSan and ASan+UBSan, and under a starved
stress. Fail-fast claims nothing it can return from in production builds. The Debouncer's
lock order holds on every path, including a `finish()` racing live touches and re-entrant
touches from the fire path. The logger's pending count is exact. Xapiand never runs a
past-horizon operation, and the in-place autocommit runs while the shard is still busy, so
nothing else can check it out.

One overclaim must be fixed before Xapiand lands (CR1). After `finish()`, a touch on a key
whose call is already pending returns `scheduled`, and nothing ever runs that call. So "no
longer dropped at shutdown" holds only for keys with no pending call. The data is still
committed at pool close, so this is a false claim, not data loss.

## What was exercised

| Check | Result |
|---|---|
| Model, the six fixed-width configurations, five mutants, main anchors (`run_fixed.sh`) | identical to the developer's `results-fixed.txt`: no violation in all six; `no_epoch` FV, `no_recheck` LOSS, `no_prev_hold` FV, `no_window_cap` UAF, `no_advance_gate` FV |
| Model, fail-fast (`run_abort.sh`) | no violation with `abort=1` on window and main; `abort_after_reserve` reaches HOLE on both |
| New: memory-order mutants (`stash_model_review.py`) | `recheck_before_count` (producer's re-read before its count is visible) reaches UAF; `late_window_publish` (walk's count scan before its window store is visible) reaches FV. Both seq_cst pairs are load-bearing |
| New interleaving: an epoch read racing a window move during the refresh's scan, and a stray (an epoch read two moves stale, so same parity as the live epoch) beside a registered far insert of that parity | reached 270,169 / 314,524 / 23,856 times (slots=1, wide=1, 10.6M states) and 151,812 / 73,285 / 6,370 (slots=2, 2.7M states): no violation |
| Three producers (slots=1, leads 0,2,4) | truncated at 30M states, no violation in what was explored (not a proof) |
| `stash` `test.cc`, plain, TSan, ASan+UBSan (Homebrew clang 23) | pass, including the forked `test_alloc_failure` (sites `PPPT` and `PTT`) |
| `stash_concurrent`, TSan (4 producers) and ASan+UBSan (8), lead 0 and 60, 3 runs each | every task fired once, no reports |
| Far path actually exercised (`concurrent_counts.cc`, 8 producers, 1 s) | far inserts 2.1M at lead 0, 3.8M at lead 60; far-pin holds 5-8 per run |
| Starved stress, lead 0: `test/stress.sh`, 20 runs, 12 producers, 4 hogs, nice 19 (light hold) | 0 failed, 0 lost of 142,729,036 |
| scheduler ctest, plain, TSan, ASan+UBSan (stash and threadpool at their pins) | pass (`no_memory` skipped under TSan, by design) |
| Debouncer probe under TSan, 3 runs: 4 touchers on 64 keys, throttlers, re-entrant touches from the fire path, `finish()` racing live touches | no race, no lock-order report |
| logger ctest, TSan and ASan+UBSan | pass |
| Xapiand `shard.cc`, `wal.cc`, `handler.cc`, `manager.cc`, `-fsyntax-only` against the pinned stash, scheduler and logger (Release flags) | compile; no new warnings from the changed headers (stash's `Depth` shadow warning predates the branch) |
| Xapiand `tests/test_debounce_or_run.cc`, ASan+UBSan | 18 checks pass |

## Checked and holding

1. **The handshake (stash.h:236-256, 323-371).** A far producer adds one with a seq_cst
   fetch_add, then re-reads `atom_window` with a seq_cst load. The walk stores `atom_window`
   (seq_cst) before its seq_cst scan of the previous epoch's counts. Either the scan sees the
   increment, or the re-read sees the new window and the producer retracts. The first window
   load is relaxed, which is safe because only the seq_cst re-read decides registration. The
   retract's relaxed `fetch_sub` is an RMW, so it continues the release sequence of a
   registered producer's release decrement; a scan that reads zero still synchronizes with
   that producer's fill. Windows strictly increase, so there is no ABA. A stray increment on
   the live parity only pins conservatively, and the model shows that interleaving is
   reached. When the floor overtakes a pinned window, a far key below the floor is filed
   unclamped at its own key, which stays at or above the cap (`window <= key`), and the model
   covers it (`floor_past_far_key=4`).
2. **Fail-fast (stash.h:972-1022).** The value box is built before the claim at :984, so a
   failure there returns `no_memory` with nothing claimed. Only the chunk or chain
   materialization at :987-992 runs after the claim, and it terminates. The levels above
   allocate before descending. After the claim, the bound CAS and the fill CAS cannot throw.
   The forked test enumerates every allocation site by count: fresh leaf `PPPT`, full leaf
   chunk `PTT`. The one exception is a trace build (CR3).
3. **The Debouncer's lock order.** The order is the status mutex, then the scheduler's
   `mtx`, and only through `debounce` → `add` → notify. The fire path (debouncer.h:158-221)
   and `release` drop the status mutex before every `add`. `finish()` takes `mtx` alone. The
   wheel thread holds `mtx` only around its wait and dispatches without it. The TSan probe
   agrees. After a refused touch nothing stays pending:
   - a fresh key or a throttled one is erased (debouncer.h:335-338 and :344-348);
   - a refused re-arm runs the call or erases the key (:190-216);
   - a bad copy of the arguments keeps the pending call's earlier arguments (:316, tested in
     `no_memory.cc`).
4. **The logger (logger.cc:582-602, 690-741).** A refusal resets `scheduled` and undoes
   `pending`, and `~Logging` decrements only when `scheduled` is set, so the count is exact
   (tested by `pending_count()` after every refusal kind). A future `clears` line is never
   written at refusal. Its cancel point writes it only once its deadline has passed. Fast
   watchdogs stay silent through both a handle drop and `clear()`.
5. **Xapiand.** `debounce_or_run` (src/debounce_or_run.h:26-39) never calls `run_now` on
   `past_horizon`, and the unit test fails if it does. The in-place commit runs inside
   `checkin` before `_busy` is cleared (pool.cc:403, then :415), so the shard is still
   exclusively held. Every other code path commits a checked-out shard the same way.
   `checkin`'s endpoint mutex is not held across the call.

## Findings

### CR1: After `finish()`, a touch on a pending key returns `scheduled` and the call never runs. Condition (C1).

Probe `scheduler/.scratch/probes/debouncer_probe.cc`, the `shutdown` part. It schedules key 1,
calls `finish()`, then touches key 1 again and touches a fresh key 2:
`before=scheduled pending-after=scheduled fresh-after=stopping ran=0 pending_keys=1`. In the
stress part, 58 to 61 of 64 keys stay pending forever after `finish()`.

Why: `ThreadedScheduler::finish` stops the pool's workers (scheduler.h:445). The wheel then
claims the pending task, which a stopped pool never runs, and the status entry is never
released. A later touch takes the pending branch (debouncer.h:323-327) and returns
`scheduled` without checking `ending`. So `debounce_or_run`'s `stopping` fallback misses
exactly the keys being written when the committer or fsyncher stops.

Xapiand's changelog bullet ("Autocommits and WAL fsyncs requested during shutdown are no longer
dropped", changelog.md:28) is false for those keys. Data is not lost:
`ShardEndpoint::finish` closes writables with a commit (pool.cc:469), after the committer
stops (manager.cc:1238 before :1325). An fsync lost this way matters only on a power loss.
This is pre-existing (main dropped every shutdown touch). It is a condition, not a blocker,
because the new `schedule_result` contract ("only `scheduled` fires") makes it a silent drop
that the owner asked this series to remove.

Fix: in the pending branch, once the scheduler is stopping (a `noexcept` accessor on
`BaseScheduler::ending`), erase the status and return `stopping`. The pending task then
finds itself superseded, and `debounce_or_run` runs the call with the latest arguments. Add
the probe's shutdown case to `test_debouncer_refusals`. Otherwise narrow the changelog and
the API docs to keys without a pending call.

### CR2: A touch on a pending call is never checked against the horizon, and a later past-horizon touch drops the earlier in-horizon call. For the architect.

debouncer.h:323-327 moves `target` without checking the horizon and returns `scheduled`. If
`target` is past the horizon, the fire-path re-arm is refused, and :202-205 drops the whole
call, including work an earlier touch scheduled within the horizon. `test_debouncer_refusals`
asserts exactly this (48 h force timeouts; the touch for value 10 is dropped along with 11).
It is reachable only when `max_wakeup_time` (delay plus force timeout) nears 24 h, which no
Xapiand debouncer configures. It is reported only through `L_DEBUG_HOOK`, which is a no-op
by default. Decide whether a touch should report `past_horizon` for a target beyond the
horizon, or keep this and document it as the caller's error.

### CR3: In a trace build, a throwing `L_STASH` after the claim returns `no_memory` with a hole. Condition (C3, cheap).

stash.h:993 traces after the claim at :984, outside the `try` at :987-992. A trace header
whose `L_STASH` allocates can throw `std::bad_alloc` there. `add` catches it and returns
`no_memory` (:817-821), leaving the claimed slot unfilled. Probe
`stash/.scratch/probes/trace_after_claim.cc`: `first add=2 (no_memory) ... first_valid=3`,
stuck behind the hole, so later adds go `past_horizon`. Production builds stub the traces out
(Xapiand's `xapiand_stash_trace.h`; the bundled `stash_trace.h`), so only
`examples/colored_trace`-style builds are exposed. Move the trace before the claim, or inside
the `try` so it terminates, and correct ARCHITECTURE's "an injected trace header that
throws ... would terminate", which is wrong for `add`.

### CR4: Xapiand's in-place autocommit catches only `Exception` and `Xapian::Error`, inside a `noexcept` `checkin`. Note.

shard.cc:751-759. If `commit` throws `std::bad_alloc` (the likely companion of the
`no_memory` path) or another `std::exception`, the process terminates through the `noexcept`
`ShardEndpoint::checkin` (pool.cc:382). This predates the branch: the old `debounce` could
throw `bad_alloc` out of the same `noexcept`, and `committer_commit` catches the same two
types. Also, a failing commit closes the shard (shard.cc:844-848) after `checkin` has
checked `is_closed()` (pool.cc:398), so `writable` keeps a closed shard instead of being
reset as the committer path would. Consider catching `std::exception` and rechecking
`is_closed()` after the call.

### CR5: Stale ledger text. Note.

The scheduler ledger's "For reviewers" bullet (docs/status/perf-lazy-rearm.md:57) still
describes DW11 as a hole after the claim. Since a080a1e that case terminates. "Done" still
names stash d234d24 for the downstream compile.

### CR6: A refused touch consumes rvalue arguments. Note.

`Tuple call_args(std::forward<Args>(args)...)` (debouncer.h:316) moves from rvalue
arguments before the add can be refused. So a direct caller cannot run the call itself on
`stopping` or `no_memory`. `debounce_or_run` passes lvalues and is unaffected. A `Tuple`
whose move-assignment throws part-way at :326 leaves mixed arguments pending while returning
`no_memory`. State both in docs/API.md.

### CR7: The window wraps at the top of the key space. Note.

stash.h:333-338: once `window` is `max_key`, the next move computes `window + 1 == 0`, and the
cap at `window` would hold `first_valid` at 0. It is unreachable with steady-clock
nanosecond keys (about 2^50). Saturating instead (never moving past `max_key - 1`, or never
opening the far path there) would make the limit explicit.

## Blocking items

None.

## Conditions

1. **C1 (CR1), before Xapiand lands.** A touch on a pending key after `finish()` returns
   `stopping` (with a test), or the Xapiand changelog and the scheduler API docs are narrowed
   to keys without a pending call.
2. **C2, landing order and pins.** Each pin names an unlanded branch commit:
   - scheduler pins stash a080a1e;
   - logger pins scheduler 5319dc5;
   - Xapiand pins all three.

   Land stash, repin scheduler to stash's `main` commit, land scheduler, then do the same for
   logger and Xapiand. Delete no branch before the pins that name it move, because a rebased
   or deleted branch commit cannot be fetched.
3. **C3 (CR3), stash.** Move the post-claim `L_STASH` before the claim or into the `try`, and
   correct the ARCHITECTURE sentence.

CR2 goes to the architect. CR4 to CR7 are follow-up notes.

## Probes, harnesses and mutants (kept)

- `review-wt/stash/.scratch/model/`:
  - `stash_model.py` and `run_*.sh` (the developer's, copied);
  - `stash_model_review.py`, the developer's model plus the `recheck_before_count` and
    `late_window_publish` mutants and the coverage counters (`race_inc_during_scan`,
    `stray_two_moves`, `stray_with_legit_same_parity`);
  - results in `results-fixed-review.txt`, `results-abort-review.txt`,
    `results-review-new1.txt`, `results-review-new2.txt`, `results-review-3producers.txt` and
    `results-review-order-mutants.txt`.
- `review-wt/stash/.scratch/probes/concurrent_counts.cc`: `test/concurrent.cc` with the far
  and hold stat hooks counted.
- `review-wt/stash/.scratch/probes/trace_after_claim.cc` (CR3).
- `review-wt/stash/.scratch/logs-stress-lead0.txt`: the starved stress.
- `review-wt/scheduler/.scratch/probes/debouncer_probe.cc` (CR1 and the TSan lock-order
  stress). Build it against the scheduler's test flags plus `libthreadpool.a` and
  `librandom.a`.
- `review-wt/xapiand/.scratch/syntax-*.log`: the `-fsyntax-only` runs.
- Dependency worktrees used for offline builds: `review-wt/{threadpool,random,term-color,static-string}`
  at the scheduler's and logger's pins.

Build directories were removed. Rebuild with `FETCHCONTENT_SOURCE_DIR_*` pointing at the
review worktrees and `FETCHCONTENT_FULLY_DISCONNECTED=ON`.

## Not run

- Linux, FreeBSD, and arm64 runs, apart from what GitHub CI ran for the three library heads.
- A full Xapiand build, its test suite, and the docs E2E suite. Nothing exercises the
  in-place autocommit or the WAL fsync fallback against a real stopping debouncer.
- TSan on the scheduler's `no_memory` test (excluded by design).
- An exhaustive three-producer model (truncated at 30M states).
- The developer's 40-run starved stress at lead 60.
- Any benchmark or A/B.
