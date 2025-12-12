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

# Nettoyage éventuel d'une exécution précédente
if [ -d "$rundir" ]; then
    rm -rf "$rundir"
fi

mkdir -p "$rundir"
"$erraid_bin" -r "$rundir" &
daemon_pid=$!

sleep 1

minutes_mask="0FFFFFFFFFFFFFF"
hours_mask="00000F"
weekdays_mask="7F"

echo "[e2e] création tâche simple"
create_simple_output="$("$tadmor_bin" -p "$pipes_dir" -c -m "$minutes_mask" -H "$hours_mask" -w "$weekdays_mask" /bin/echo "hello end-to-end")"
echo "$create_simple_output"
simple_task_id="$(echo "$create_simple_output" | awk -F':' '/task_id/ {gsub(/[^0-9]/, "", $2); print $2}')"

echo "[e2e] création tâche séquentielle"
create_sequence_output="$("$tadmor_bin" -p "$pipes_dir" -s -m "$minutes_mask" -H "$hours_mask" -w "$weekdays_mask" /bin/echo "first" -- /bin/sh -c "echo second" )"
echo "$create_sequence_output"
sequence_task_id="$(echo "$create_sequence_output" | awk -F':' '/task_id/ {gsub(/[^0-9]/, "", $2); print $2}')"

sleep 2

echo "[e2e] liste des tâches"
"$tadmor_bin" -p "$pipes_dir" -l

if [ -n "$simple_task_id" ]; then
    echo "[e2e] récupération stdout/stderr tâche simple"
    "$tadmor_bin" -p "$pipes_dir" -o "$simple_task_id" || true
    "$tadmor_bin" -p "$pipes_dir" -e "$simple_task_id" || true
    echo "[e2e] historique tâche simple"
    "$tadmor_bin" -p "$pipes_dir" -x "$simple_task_id" || true
fi

if [ -n "$sequence_task_id" ]; then
    echo "[e2e] suppression tâche séquentielle"
    "$tadmor_bin" -p "$pipes_dir" -r "$sequence_task_id" || true
fi

if [ -n "$simple_task_id" ]; then
    echo "[e2e] suppression tâche simple"
    "$tadmor_bin" -p "$pipes_dir" -r "$simple_task_id" || true
fi

"$tadmor_bin" -p "$pipes_dir" -q

wait "$daemon_pid" || true
