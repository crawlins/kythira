#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Runs an M1 campaign (.kiro/specs/cairn-chaos-testing/ task 11): up to N
# election-safety projects against the seeded-vote-term chaos_node, one
# after another, and writes a per-run summary. It is the driver that ran
# the second Qwen3.8 campaign, with its two logging bugs fixed.
#
# Usage: m1-campaign.sh --out DIR [--runs N] [--hours H] [--deadline EPOCH]
#   --out       where run-<i>/ directories, driver.log and SUMMARY.csv go
#   --runs      projects to run, default 5
#   --hours     each run's wall-clock limit, default 2
#   --deadline  epoch second the model host must be released by (its cost
#               cap); a run is not started with less than --hours left
#
# Per run:
#   1. Bootstrap. start-run.sh starts the project; it counts as bootstrapped
#      once the dispatcher reports a reason/explore step for it. If the
#      dispatcher logs "bootstrap parse failed" three times for it, or 20
#      minutes pass, the stack is killed and the project started again, up
#      to three attempts. A run that never bootstraps ends the campaign
#      (the model is not usable as served, so further runs only bill).
#   2. Explore, polling every 30 s, until the schedule log holds an
#      election_safety violation, the project completes, or --hours pass.
#   3. Save the schedule log, the timeline export, the dispatcher log and
#      the llm-tunnel log (which carries json_repair.py's notes, including
#      the text of every final answer that was not bare JSON), count the
#      model's tokens for this run, append to SUMMARY.csv, kill the stack.
#
# Environment: as start-run.sh (CAIRN_LLM_SSH, CAIRN_LLM_SSH_KEY,
# KYTHIRA_CHAOS_NODE_IMAGE, KYTHIRA_CONTAINER_RUNTIME, CONTAINER_SOCKET,
# CAIRN_RUN, CAIRN_PANEL_PORT). KYTHIRA_CHAOS_NODE_IMAGE defaults to
# kythira-chaos-node:seeded-vote-term. Token counts are read over SSH from
# ~/server.log on the model host (scripts/cairn/llm-gpu-setup.sh writes it).

set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
export CAIRN_RUN="${CAIRN_RUN:-kythira-cairn}"
export KYTHIRA_CHAOS_NODE_IMAGE="${KYTHIRA_CHAOS_NODE_IMAGE:-kythira-chaos-node:seeded-vote-term}"
panel="http://127.0.0.1:${CAIRN_PANEL_PORT:-7777}"
tool="$CAIRN_RUN-tool-server"
disp="$CAIRN_RUN-dispatcher"
tunnel="$CAIRN_RUN-llm-tunnel"

out="" runs=5 hours=2 deadline=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        --runs) runs="$2"; shift 2 ;;
        --hours) hours="$2"; shift 2 ;;
        --deadline) deadline="$2"; shift 2 ;;
        *) echo "m1-campaign.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$out" ] || { echo "m1-campaign.sh: --out is required" >&2; exit 2; }
limit=$((hours * 3600))
mkdir -p "$out"
cd "$repo" || exit 1

log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$out/driver.log"; }

dispatcher_log() { "$runtime" logs "$disp" 2>&1; }

parse_failures() {
    dispatcher_log | grep "project=$1" | grep -c 'bootstrap parse failed'
}

advanced() {
    dispatcher_log | grep "project=$1" |
        grep -Eq 'phase=reason|phase=explore|tasks.reason|dispatched reason|dispatched explore|outcome=fact'
}

project_of() { awk '/^project:/{print $2}' "$1"; }

violation() {
    python3 - "$1" <<'PY'
import json, sys
try:
    for line in open(sys.argv[1]):
        try:
            text = json.dumps(json.loads(line))
        except ValueError:
            continue
        if '"violations"' in text and '"election_safety"' in text:
            sys.exit(0)
except FileNotFoundError:
    pass
sys.exit(1)
PY
}

# The model host's running token total; per-run counts are differences.
model_tokens() {
    [ -n "${CAIRN_LLM_SSH:-}" ] || { echo ""; return; }
    ssh ${CAIRN_LLM_SSH_KEY:+-i "$CAIRN_LLM_SSH_KEY"} -o ConnectTimeout=10 \
        -o StrictHostKeyChecking=accept-new "$CAIRN_LLM_SSH" \
        'grep -hoE "[0-9]+ tokens" ~/server.log 2>/dev/null | awk "{s+=\$1} END {print s+0}"' \
        2>/dev/null || echo ""
}

stop_stack() {
    scripts/cairn/kill.sh >/dev/null 2>&1
    pkill -f 'kill.sh --after' 2>/dev/null
}

# Everything a run leaves behind, saved before its stack is killed.
save_logs() {
    dispatcher_log > "$1/dispatcher.log" || true
    "$runtime" logs "$tunnel" > "$1/llm-tunnel.log" 2>&1 || true
}

echo "run,found,first_violation_min,chaos_calls,bootstrap_restarts,run_min,tokens,final_status" > "$out/SUMMARY.csv"
for i in $(seq 1 "$runs"); do
    if [ -n "$deadline" ] && [ $((deadline - $(date +%s))) -lt "$limit" ]; then
        log "BUDGET: less than ${hours}h to the deadline; not starting run $i"
        break
    fi
    rd="$out/run-$i"
    mkdir -p "$rd"
    run0=$(date +%s)
    tok0=$(model_tokens)
    boot_ok=0 restarts=0 p=""
    for attempt in 1 2 3; do
        scripts/cairn/start-run.sh election-safety --hours "$hours" --seed "$i" \
            > "$rd/start-$attempt.out" 2>&1
        p=$(project_of "$rd/start-$attempt.out")
        if [ -z "$p" ]; then
            log "RUN $i attempt $attempt: start failed"
            stop_stack
            restarts=$((restarts + 1))
            continue
        fi
        log "RUN $i attempt $attempt: project=$p"
        while [ $(($(date +%s) - run0)) -lt 1200 ]; do
            advanced "$p" && { boot_ok=1; break; }
            if [ "$(parse_failures "$p")" -ge 3 ]; then
                log "RUN $i attempt $attempt: bootstrap parse failed x3; restarting"
                break
            fi
            sleep 20
        done
        [ "$boot_ok" = 1 ] && break
        save_logs "$rd"
        mv "$rd/dispatcher.log" "$rd/dispatcher-attempt-$attempt.log" 2>/dev/null
        mv "$rd/llm-tunnel.log" "$rd/llm-tunnel-attempt-$attempt.log" 2>/dev/null
        stop_stack
        restarts=$((restarts + 1))
        sleep 3
    done
    if [ "$boot_ok" != 1 ]; then
        log "RUN $i: could not bootstrap after three attempts; ending the campaign"
        tok1=$(model_tokens)
        echo "$i,0,NA,0,$restarts,$((($(date +%s) - run0) / 60)),$((${tok1:-0} - ${tok0:-0})),BOOTSTRAP_FAIL" >> "$out/SUMMARY.csv"
        stop_stack
        break
    fi
    log "RUN $i: bootstrapped (restarts=$restarts); exploring"
    found=0 fvmin=""
    while true; do
        el=$(($(date +%s) - run0))
        "$runtime" exec "$tool" cat /runs/current/schedule.jsonl > "$rd/schedule.jsonl" 2>/dev/null || true
        if violation "$rd/schedule.jsonl"; then
            found=1 fvmin=$((el / 60))
            log "RUN $i: election_safety FOUND at about ${fvmin}m"
            break
        fi
        st=$(curl -fsS "$panel/projects/$p" 2>/dev/null |
            python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))' 2>/dev/null)
        if [ "$st" = complete ] || [ "$st" = completed ]; then
            log "RUN $i: status=$st"
            break
        fi
        [ "$el" -gt "$limit" ] && { log "RUN $i: ${hours}h limit"; break; }
        sleep 30
    done
    run_min=$((($(date +%s) - run0) / 60))
    "$runtime" exec "$tool" cat /runs/current/schedule.jsonl > "$rd/schedule.jsonl" 2>/dev/null || true
    calls=$(grep -c . "$rd/schedule.jsonl" 2>/dev/null || echo 0)
    curl -fsS "$panel/projects/$p/export?format=timeline" -o "$rd/timeline.json" 2>/dev/null || true
    save_logs "$rd"
    tok1=$(model_tokens)
    tokens=NA
    [ -n "$tok0" ] && [ -n "$tok1" ] && tokens=$((tok1 - tok0))
    status=nofind
    [ "$found" = 1 ] && status=FOUND
    log "RUN $i DONE found=$found first_violation_min=${fvmin:-NA} calls=$calls restarts=$restarts run_min=$run_min tokens=$tokens"
    echo "$i,$found,${fvmin:-NA},$calls,$restarts,$run_min,$tokens,$status" >> "$out/SUMMARY.csv"
    stop_stack
    sleep 5
done
log "CAMPAIGN DONE"
column -s, -t "$out/SUMMARY.csv" 2>/dev/null | tee -a "$out/driver.log"
