#!/bin/sh
# Window variant and its mutants, two at a time (each run holds the visited set in memory).
cd "$(dirname "$0")"
run() { nice -n 19 python3 stash_model.py "$@"; }
{
run window none sched slots=1 leads=0,3 tmax=5 wide=2 &
run window none cont slots=1 leads=0,3 tmax=5 wide=2 & wait
run window none sched slots=2 leads=0,2 tmax=4 wide=2 &
run window none sched slots=1 leads=0,3 tmax=5 period=2 wide=2 & wait
run window none sched slots=1 leads=0,3 tmax=5 wide=1 &
run window none sched slots=1 leads=1,4 tmax=6 wide=3 & wait
for m in no_epoch no_recheck no_prev_hold no_window_cap no_advance_gate shrink_nonmonotone; do
  run window $m sched slots=1 leads=0,3 tmax=5 wide=3
done
run main none sched slots=2 tmax=4 &
run main none cont slots=2 tmax=4 & wait
} 2>&1
