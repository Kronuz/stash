#!/bin/sh
# Fixed-width window (no adaptive shrink), its mutants, and main as the anchor.
# Two at a time at nice 19 (each run holds its visited set in memory).
cd "$(dirname "$0")"
run() { nice -n 19 python3 stash_model.py "$@"; }
{
run window none sched slots=1 leads=0,3 tmax=5 wide=2 &
run window none cont slots=1 leads=0,3 tmax=5 wide=2 & wait
run window none sched slots=2 leads=0,2 tmax=4 wide=2 &
run window none sched slots=1 leads=0,3 tmax=5 period=2 wide=2 & wait
run window none sched slots=1 leads=0,3 tmax=5 wide=1 &
run window none sched slots=1 leads=1,4 tmax=6 wide=3 & wait
run window no_epoch sched slots=1 leads=0,3 tmax=5 wide=3 &
run window no_recheck sched slots=1 leads=0,3 tmax=5 wide=3 & wait
run window no_prev_hold sched slots=1 leads=0,3 tmax=5 wide=3 &
run window no_window_cap sched slots=1 leads=0,3 tmax=5 wide=1 & wait
run window no_advance_gate sched slots=1 leads=0,3 tmax=5 wide=1 &
run main none sched slots=2 tmax=4 & wait
run main none cont slots=2 tmax=4
} 2>&1
