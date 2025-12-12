#!/bin/sh
set -eu

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
rundir="$project_root/run"
pipes_dir="$rundir/pipes"

erraid_bin="$project_root/erraid"
tadmor_bin="$project_root/tadmor"

if [ ! -x "$erraid_bin" ] && [ -x "$project_root/build/erraid/erraid" ]; then
    erraid_bin="$project_root/build/erraid/erraid"
fi
if [ ! -x "$tadmor_bin" ] && [ -x "$project_root/build/tadmor/tadmor" ]; then
    tadmor_bin="$project_root/build/tadmor/tadmor"
fi

if [ ! -x "$erraid_bin" ]; then
    echo "[e2e] Binaire erraid introuvable. Lancez 'make'." >&2
    exit 1
fi
if [ ! -x "$tadmor_bin" ]; then
    echo "[e2e] Binaire tadmor introuvable. Lancez 'make'." >&2
    exit 1
fi

cleanup() {
    if [ "${daemon_pid-}" != "" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

mkdir -p "$rundir"
"$erraid_bin" -r "$rundir" &
daemon_pid=$!

sleep 1

minutes_mask="0FFFFFFFFFFFFFF"
hours_mask="00000F"
weekdays_mask="7F"

"$tadmor_bin" -p "$pipes_dir" -c -m "$minutes_mask" -H "$hours_mask" -w "$weekdays_mask" /bin/echo "hello end-to-end"

sleep 2

"$tadmor_bin" -p "$pipes_dir" -l

created_task_id="$("$tadmor_bin" -p "$pipes_dir" -l | awk -F':' '/task_id/ {gsub(/[^0-9]/, "", $2); if ($2 != "") {print $2; exit}}')"

if [ "$created_task_id" != "" ]; then
    "$tadmor_bin" -p "$pipes_dir" -x "$created_task_id" || true
    "$tadmor_bin" -p "$pipes_dir" -r "$created_task_id" || true
fi

"$tadmor_bin" -p "$pipes_dir" -q

wait "$daemon_pid" || true
