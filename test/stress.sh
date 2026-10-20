#!/usr/bin/env bash
# Repeats stash_concurrent and tallies lost tasks, optionally against nice'd
# busy loops so producers get preempted mid-insert (macOS has no taskset).
#
#   test/stress.sh BINARY [RUNS] [PRODUCERS] [SECONDS] [LEAD_MS] [HOGS]
#   test/stress.sh build/stash_concurrent 20 24 2.0 0 16
#
# Exits nonzero if any run lost, double-fired, crashed, or hung.
set -u
bin=${1:?usage: test/stress.sh BINARY [RUNS] [PRODUCERS] [SECONDS] [LEAD_MS] [HOGS]}
runs=${2:-20}
producers=${3:-16}
seconds=${4:-2.0}
lead=${5:-0}
hogs=${6:-0}

hog_pids=()
cleanup() { for p in "${hog_pids[@]+"${hog_pids[@]}"}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT
for ((h = 0; h < hogs; h++)); do
	nice -n 10 sh -c 'while :; do :; done' &
	hog_pids+=($!)
done

failed=0 lost_total=0 added_total=0
for ((r = 1; r <= runs; r++)); do
	out=$("$bin" "$producers" "$seconds" 32 0 "$lead" 2>&1)
	rc=$?
	added=$(sed -n 's/.*added=\([0-9]*\).*/\1/p' <<<"$out")
	fired=$(sed -n 's/.*fired=\([0-9]*\).*/\1/p' <<<"$out")
	lost=$(( ${added:-0} - ${fired:-0} ))
	added_total=$(( added_total + ${added:-0} ))
	lost_total=$(( lost_total + lost ))
	if ((rc != 0)); then
		failed=$((failed + 1))
		echo "run $r: FAIL rc=$rc added=${added:-?} fired=${fired:-?} lost=$lost"
		grep -E 'RESULT|WATCHDOG|ERROR|WARNING' <<<"$out" | head -3
	else
		echo "run $r: ok added=$added"
	fi
done
echo "stress: $runs runs, producers=$producers seconds=$seconds lead=${lead}ms hogs=$hogs: $failed failed, $lost_total lost of $added_total"
((failed == 0))
