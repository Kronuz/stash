#!/bin/sh
# DW11: producers that fail to allocate (abort=1) before the leaf reserve, on the
# fixed-width window and main, the HOLE invariant on the unchanged configs, and the
# mutant that fails after reserving (must leave a HOLE).
cd "$(dirname "$0")"
run() { nice -n 19 python3 stash_model.py "$@"; }
{
run window none sched slots=1 leads=0,3 tmax=5 wide=2 abort=1 &
run window none cont slots=1 leads=0,3 tmax=5 wide=2 abort=1 & wait
run window none sched slots=2 leads=0,2 tmax=4 wide=2 abort=1 &
run main none sched slots=2 tmax=4 abort=1 & wait
run window abort_after_reserve sched slots=1 leads=0,3 tmax=5 wide=2 abort=1 &
run main abort_after_reserve sched slots=2 tmax=4 abort=1 & wait
run window none sched slots=1 leads=0,3 tmax=5 wide=2 &
run main none cont slots=2 tmax=4 & wait
} 2>&1
