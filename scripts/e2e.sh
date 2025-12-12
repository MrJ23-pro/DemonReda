#!/bin/sh
set -eu

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
rundir="$project_root/run"
pipes_dir="$rundir/pipes"

cleanup() {
    if [ "${daemon_pid-}" != "" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

mkdir -p "$rundir"
"$project_root/erraid" -r "$rundir" &
daemon_pid=$!

sleep 1

minutes_mask="0FFFFFFFFFFFFFF"
hours_mask="00000F"
weekdays_mask="7F"

"$project_root/tadmor" -p "$pipes_dir" -c -m "$minutes_mask" -H "$hours_mask" -w "$weekdays_mask" /bin/echo "hello end-to-end"

sleep 2

"$project_root/tadmor" -p "$pipes_dir" -l

created_task_id="$("$project_root/tadmor" -p "$pipes_dir" -l | awk -F':' '/task_id/ {gsub(/[^0-9]/, "", $2); if ($2 != "") {print $2; exit}}')"

if [ "$created_task_id" != "" ]; then
    "$project_root/tadmor" -p "$pipes_dir" -x "$created_task_id" || true
    "$project_root/tadmor" -p "$pipes_dir" -r "$created_task_id" || true
fi

"$project_root/tadmor" -p "$pipes_dir" -q

wait "$daemon_pid" || true
