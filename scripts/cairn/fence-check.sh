#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Brings up docker/cairn/cairn-compose.yml and checks the fence
# (.kiro/specs/cairn-chaos-testing/ tasks 8.2-8.4):
#
#   1. chaos_ctl, run in the tool server, drives the cluster through the
#      filtering proxy: init, a workload, kill, restart, a partition and its
#      heal, reset, and every checker clean.
#   2. The tool server can do nothing else with the runtime (ps, run, exec
#      of anything but tc/iptables, any container outside the run) and has
#      no route off the host.
#   3. fence-probe, sitting where a worker sits, reaches the model hosts
#      through the egress proxy and nothing else (fence_probe.py).
#   4. scripts/cairn/kill.sh takes all of it down, leaving no container of
#      the run behind.
#   5. If $KYTHIRA_CHAOS_CTL names a host chaos_ctl, the run from step 1
#      replays on the host's cluster (replay --layout chaos3), as a finding
#      made inside the fence would.
#
# Needs kythira-chaos-node:dev and kythira-cairn-tool-server:dev built
# (cmake --build build --target docker-cairn-fence-tests does both, then
# runs this). Uses $KYTHIRA_CONTAINER_RUNTIME and $KYTHIRA_COMPOSE_COMMAND
# as kill.sh does; for rootless Podman set CONTAINER_SOCKET to its API
# socket. CAIRN_PROBE_ALLOWED / CAIRN_EGRESS_ALLOW override the model hosts.
# Exits non-zero on the first failed step, after tearing down.

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
export CAIRN_RUN="${CAIRN_RUN:-kythira-cairn-fence-check}"
tool="$CAIRN_RUN-tool-server"
# The runs volume outlives kill.sh, as a run's artifact should.
run_dir="/runs/fence-check-$(date +%Y%m%dT%H%M%S)-$$"

step() { echo; echo "== $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "-- logs (failed with $status)"
        "${compose[@]}" -f "$file" logs --tail 40 tool-server container-api-proxy egress-proxy \
            2>&1 || true
    fi
    "$repo/scripts/cairn/kill.sh" || true
    exit "$status"
}

# chaos_ctl in the tool server; its last line is its JSON result.
ctl() {
    "$runtime" exec "$tool" chaos_ctl "$@" --run-dir "$run_dir"
}
ok() {
    local out
    out="$(ctl "$@")" || fail "chaos_ctl $*: $out"
    echo "chaos_ctl $1: ${out:0:300}"
    case "$out" in
        *'"violations":[{'*) fail "chaos_ctl $1 reported a violation: $out" ;;
    esac
}
refused() {
    local what="$1" out
    shift
    if out="$("$runtime" exec "$tool" "$@" 2>&1)"; then
        fail "the tool server could $what: $out"
    fi
    echo "refused as it should be: $what (${out:0:160})"
}

"$repo/scripts/cairn/kill.sh" > /dev/null || true
trap cleanup EXIT

step "build the proxies and bring the stack up"
"${compose[@]}" -f "$file" --profile probe build container-api-proxy egress-proxy fence-probe
"${compose[@]}" -f "$file" up -d

step "chaos_ctl drives the cluster through the filtering proxy"
image_id="$("$runtime" image inspect --format '{{.Id}}' kythira-chaos-node:dev)"
out="$(ctl init --layout cairn --seed 1 --image-id "$image_id")" || fail "chaos_ctl init: $out"
leader=""
for _ in $(seq 1 60); do
    out="$(ctl cluster.status 2>/dev/null || true)"
    if [[ "$out" == *'"leader":'[0-9]* ]]; then
        leader=yes
        break
    fi
    sleep 1
done
[ -n "$leader" ] || fail "no leader within 60s: $out"
ok workload.start '{"rate":40,"concurrency":3}'
sleep 2
ok fault.kill '{"node":3}'
sleep 1
ok fault.restart '{"node":3}'
ok fault.partition '{"groups":[[1],[2,3]]}'
sleep 2
ok fault.partition '{"heal":true}'
ok run.reset
sleep 1
ok workload.stop
check="$(ctl check.run '{"checks":["raft_safety","linearizability","liveness"]}')" \
    || fail "check.run: $check"
echo "check.run: ${check:0:400}"
python3 - "$check" <<'PY' || fail "a checker is not ok: $check"
import json, sys
result = json.loads(sys.argv[1].strip().splitlines()[-1])["result"]
bad = [c for c in ("raft_safety", "linearizability", "liveness") if result[c]["verdict"] != "ok"]
sys.exit(f"not ok: {bad}" if bad else 0)
PY

step "the tool server can do nothing else"
refused "list containers" docker ps
refused "create a container" docker run --rm kythira-chaos-node:dev true
refused "remove a node" docker rm -f "$CAIRN_RUN-node1"
refused "run a shell in a node" docker exec "$CAIRN_RUN-node1" sh -c id
refused "exec as another user" docker exec -u 0 "$CAIRN_RUN-node1" iptables -F CHAOS
refused "kill a container outside the run" docker kill "$CAIRN_RUN-egress-proxy"
refused "inspect a container outside the run" docker inspect "$CAIRN_RUN-container-api-proxy"
refused "reach the internet" curl -sS -m 5 -o /dev/null https://example.com

local_copy=""
if [ -n "${KYTHIRA_CHAOS_CTL:-}" ]; then
    local_copy="$(mktemp -d)"
    "$runtime" cp "$tool:$run_dir" "$local_copy/run"
fi

step "fence-probe from a worker's seat"
"${compose[@]}" -f "$file" --profile probe run --rm fence-probe

step "kill.sh takes it all down"
trap - EXIT
"$repo/scripts/cairn/kill.sh"
left="$("$runtime" ps -aq --filter "label=kythira.cairn.run=$CAIRN_RUN")"
[ -z "$left" ] || fail "containers of $CAIRN_RUN survived kill.sh: $left"
for name in tool-server container-api-proxy egress-proxy; do
    if "$runtime" container inspect "$CAIRN_RUN-$name" > /dev/null 2>&1; then
        fail "$CAIRN_RUN-$name survived kill.sh"
    fi
done

if [ -n "$local_copy" ]; then
    step "the run replays on the host"
    trap '"$repo/scripts/cairn/kill.sh" > /dev/null || true' EXIT
    out="$("$KYTHIRA_CHAOS_CTL" replay --run-dir "$local_copy/run" --layout chaos3 \
        --compose "$repo/docker/docker-compose.yml" --repeat 1)" || fail "replay: $out"
    echo "replay: ${out:0:400}"
    [[ "$out" == *'"attempts":1'* && "$out" == *'"reproduced":0'* ]] || fail "replay: $out"
    rm -rf "$local_copy"
fi
echo
echo "cairn fence check passed"
