#!/bin/sh
# Mutants whose branch needs frequent window moves (wide=1), and the immediate shrink.
cd "$(dirname "$0")"
run() { nice -n 19 python3 stash_model.py "$@"; }
{
run window no_window_cap sched slots=1 leads=0,3 tmax=5 wide=1 &
run window no_advance_gate sched slots=1 leads=0,3 tmax=5 wide=1 & wait
run window shrink_immediate sched slots=1 leads=0,3 tmax=5 wide=3 &
run window shrink_immediate sched slots=1 leads=0,2 tmax=5 wide=2 & wait
} 2>&1
