#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Starts one cairn run (.kiro/specs/cairn-chaos-testing/ tasks 10-11): the
# stack in docker/cairn/cairn-compose.yml with the engine, its dispatcher
# and the tunnel to llama.cpp; a fresh run directory for the cluster; and
# one Cairn project from docker/cairn/projects/, whose origin is the
# healthy cluster's facts as cluster.status reports them. Then it arms the
# wall-clock limit (Requirement 8.3): scripts/cairn/kill.sh --after.
#
# Usage: start-run.sh <project> [--hours H] [--seed N] [--no-limit]
#   project   a file name under docker/cairn/projects/ without .json,
#             e.g. election-safety
#   --hours   the wall-clock limit, default 2
#   --seed    the run seed chaos_ctl records, default the epoch second
#   --no-limit  do not arm kill.sh (for a supervised run)
#
# Needs the images built: kythira-chaos-node (KYTHIRA_CHAOS_NODE_IMAGE
# picks the tag, e.g. kythira-chaos-node:seeded-vote-term for M1),
# kythira-cairn-tool-server:dev, kythira-cairn-worker:dev
# (docker/cairn/worker/Dockerfile) and kythira-cairn-engine:v0.2.1
# (scripts/cairn/build-engine.sh). For the model: CAIRN_LLM_SSH (user@host
# of the llama.cpp server) and CAIRN_LLM_SSH_KEY (its private key file);
# without them the llm profile is left off and the workers have no model.
# CAIRN_TARGET_FEATURES (comma-separated: peer2peer, object_store) adds the
# seed hints that apply only to a target built with those features; the
# Docker chaos_node has neither, so by default only the snapshot and
# joint-consensus hints are given (design §5).
# Uses $KYTHIRA_CONTAINER_RUNTIME and $KYTHIRA_COMPOSE_COMMAND as kill.sh
# does; for rootless Podman set CONTAINER_SOCKET to its API socket. Prints
# the project id, the run directory and the panel URL last.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
if [ -n "${KYTHIRA_COMPOSE_COMMAND:-}" ]; then
    # shellcheck disable=SC2206  # a command and its words
    compose=(${KYTHIRA_COMPOSE_COMMAND})
else
    compose=("$runtime" compose)
fi
file="$repo/docker/cairn/cairn-compose.yml"
export CAIRN_RUN="${CAIRN_RUN:-kythira-cairn}"
export CAIRN_WORKER_IMAGE="${CAIRN_WORKER_IMAGE:-kythira-cairn-worker:dev}"
node_image="${KYTHIRA_CHAOS_NODE_IMAGE:-kythira-chaos-node:dev}"
panel="http://127.0.0.1:${CAIRN_PANEL_PORT:-7777}"
tool="$CAIRN_RUN-tool-server"

usage() { sed -n '/^# Usage:/,/^#   --no-limit/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
fail() { echo "start-run: $*" >&2; exit 1; }

[ $# -ge 1 ] || usage
project="$1"
shift
hours=2
seed="$(date +%s)"
limit=1
while [ $# -gt 0 ]; do
    case "$1" in
        --hours) [[ "${2:-}" =~ ^[0-9]+$ ]] || usage; hours="$2"; shift 2 ;;
        --seed) [[ "${2:-}" =~ ^[0-9]+$ ]] || usage; seed="$2"; shift 2 ;;
        --no-limit) limit=0; shift ;;
        *) usage ;;
    esac
done
spec="$repo/docker/cairn/projects/$project.json"
[ -f "$spec" ] || fail "no project $spec"

# dispatch.yaml names the worker image and network for the default run;
# the dispatch proxy refuses anything else, so render a copy that matches.
# It is kept: the dispatcher reads it for the life of the run.
dispatch_config="$(mktemp "${TMPDIR:-/tmp}/cairn-dispatch-$CAIRN_RUN-XXXXXX.yaml")"
sed -e "s|\"kythira-cairn-worker:dev\"|\"$CAIRN_WORKER_IMAGE\"|" \
    -e "s|\"kythira-cairn-cairn_net\"|\"$CAIRN_RUN-cairn_net\"|" \
    "$repo/docker/cairn/dispatch.yaml" > "$dispatch_config"
chmod 0644 "$dispatch_config"
export CAIRN_DISPATCH_CONFIG="$dispatch_config"
export KYTHIRA_CHAOS_NODE_IMAGE="$node_image"

profiles=(--profile engine)
if [ -n "${CAIRN_LLM_SSH:-}" ]; then
    [ -r "${CAIRN_LLM_SSH_KEY:-}" ] || fail "CAIRN_LLM_SSH is set but CAIRN_LLM_SSH_KEY is not a readable key"
    profiles+=(--profile llm)
else
    echo "start-run: CAIRN_LLM_SSH unset; starting without llm-tunnel, so workers have no model" >&2
fi

"${compose[@]}" -f "$file" "${profiles[@]}" up -d

# A fresh run directory, made current for chaos_http.py.
run_dir="/runs/$project-$(date -u +%Y%m%dT%H%M%SZ)"
image_id="$("$runtime" image inspect --format '{{.Id}}' "$node_image")"
# The tool server's image has no git checkout, so chaos_ctl cannot find the
# commit itself and every schedule-log line would say "unknown" (as all of
# M1's did). The image is built from this tree, so record its HEAD, marked
# when the tree has uncommitted changes.
kythira_commit="$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$kythira_commit" != unknown ] && ! git -C "$repo" diff --quiet HEAD -- 2>/dev/null; then
    kythira_commit="$kythira_commit-dirty"
fi
"$runtime" exec -e KYTHIRA_COMMIT="$kythira_commit" "$tool" chaos_ctl init --layout cairn \
    --seed "$seed" --image-id "$image_id" --run-dir "$run_dir" > /dev/null \
    || fail "chaos_ctl init failed"
"$runtime" exec "$tool" ln -sfn "$run_dir" /runs/current

status=""
for _ in $(seq 1 60); do
    status="$("$runtime" exec "$tool" chaos_ctl cluster.status | tail -n 1 || true)"
    [[ "$status" == *'"leader":'[0-9]* ]] && break
    status=""
    sleep 1
done
[ -n "$status" ] || fail "no leader within 60s"

engine_up=""
for _ in $(seq 1 60); do
    curl -fsS -o /dev/null "$panel/projects" 2>/dev/null && engine_up=yes && break
    sleep 2
done
[ -n "$engine_up" ] || fail "the engine did not answer at $panel within 120s"
# Only now, so the dispatcher finds the engine serving from its first call.
# --no-deps: the engine is up, and compose may otherwise recreate it.
"${compose[@]}" -f "$file" "${profiles[@]}" --profile dispatch up -d --no-deps \
    dispatch-api-proxy dispatcher

body="$(python3 - "$spec" "$status" "$node_image" "$seed" "${CAIRN_TARGET_FEATURES:-}" <<'PY'
import json, sys

spec_path, status_line, image, seed, features = sys.argv[1:]
spec = json.load(open(spec_path))
status = json.loads(status_line).get("result", {})
origin = f"""You are testing a three-node Kythira Raft cluster (nodes 1, 2 and 3) for
safety bugs by injecting faults and checking invariants. Image {image}, run seed {seed}.

The only way to act on the cluster is the `chaos` command in your shell:
  chaos tools                      lists every tool and its arguments
  chaos <tool> '<json arguments>'  calls one, e.g. chaos fault.kill '{{"node":2}}'
It prints one JSON object. Every call that changes the cluster is followed
by a Raft safety check, and any violation is listed under "violations" in
that same result. Every call is recorded in the run's schedule log, which is
replayed later without you, so a finding is the exact sequence of calls.

Useful habits: start the workload (workload.start) so there is traffic to
lose; read cluster.status often; check.run runs the checkers on demand;
run.reset heals every fault and restarts stopped nodes between experiments.
Nothing outside the chaos command is reachable from here, by design.

The cluster as it started (cluster.status):
{json.dumps(status, indent=2)}"""
hints = list(spec["hints"])
for feature in filter(None, (f.strip() for f in features.split(","))):
    if feature not in spec["hints_by_feature"]:
        sys.exit(f"start-run: unknown target feature {feature!r}")
    hints += spec["hints_by_feature"][feature]
print(json.dumps({
    "title": spec["title"],
    "origin": origin,
    "goal": spec["goal"],
    "hints": [{"content": h, "creator": "kythira"} for h in hints],
}))
PY
)"
# Projects outlive kill.sh (the cairn_data volume, kept for triage) and
# stay active, and the dispatcher runs one at a time: stop any left over,
# or it would carry on with an old project against this run's cluster.
for old in $(curl -fsS "$panel/projects" | python3 -c '
import json, sys
print(" ".join(p["id"] for p in json.load(sys.stdin) if p.get("status") == "active"))'); do
    curl -fsS -o /dev/null -X PUT -H 'Content-Type: application/json' \
        --data '{"status":"stopped"}' "$panel/projects/$old/status" \
        || fail "could not stop the old project $old"
    echo "start-run: stopped the old project $old"
done

created="$(curl -fsS -X POST -H 'Content-Type: application/json' --data-binary "$body" \
    "$panel/projects")" || fail "creating the project failed"
project_id="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["project"]["id"])' "$created")"

if [ "$limit" -eq 1 ]; then
    nohup "$repo/scripts/cairn/kill.sh" --after "$((hours * 3600))" \
        > "${TMPDIR:-/tmp}/cairn-kill-$CAIRN_RUN.log" 2>&1 &
    echo "start-run: kill.sh fires in ${hours}h (pid $!)"
fi
echo "project:  $project_id ($project)"
echo "run dir:  $run_dir (in $tool)"
echo "panel:    $panel"
