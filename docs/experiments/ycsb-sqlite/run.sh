#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
experiment_dir="$PWD"
project_dir="$(cd ../../.. && pwd)"
id=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$(git -C "$project_dir" rev-parse --short=7 HEAD)}
export RUN_ID="$id"

log() {
    local f="logs/$1-$id.log"
    [[ ! -e "$f" ]] || { echo "$f exists; use another RUN_ID" >&2; return 1; }
    mkdir -p logs
    { printf '$ RUN_ID=%s ./run.sh %s\n' "$id" "$1"; "${@:2}"; } 2>&1 | sed "s|$HOME|~|g" | tee "$f"
}

run() {
    python3 "$project_dir/bench/ycsb/run.py" "$@" --output "$experiment_dir"
}

all() {
    log collect run collect --run-id "$id"
    log verify run verify
    log analyze run analyze
}

case "${1:-}" in
    collect) log collect run collect --run-id "$id" ;;
    process) log process run analyze ;;
    analyze) log analyze run analyze ;;
    verify) log verify run verify ;;
    profile) log profile run profile --run-id "$id" --sizes 10000 --workloads C,E --threads 1 --repetitions 1 --operations 1000 --timeout 180 ;;
    all) log all all ;;
    *) echo 'usage: ./run.sh collect|process|analyze|verify|profile|all' >&2; exit 2 ;;
esac
