#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The DynamoDB chaos scenario (.kiro/specs/dynamodb-compatible-api/ task
# 24): a three-node dynamodb_gateway_node cluster on RocksDB, with
# automatic splits at a small size, under SIGKILL while writers fill a
# table that has a GSI and a stream.
#
# chaos_driver.py creates the table and writes to it for WRITE_SECONDS
# while this script kills each node in turn, ROUNDS times, and starts it
# again. Every node leads some of the groups, so every group loses its
# leader at least once: while GSI entries are on their way to the index's
# groups, and while the policy splits groups that have grown (the split
# threshold is 512 KiB and a group may split again 2 s after its last
# split, so splits run all through the kills). Then the driver checks that
# no acknowledged write, index entry or stream record was lost or
# duplicated, and that a split happened.
#
# Usage:
#   run_chaos.sh compose OUT_DIR
#       the nodes and the driver in containers (compose.yml), under
#       $KYTHIRA_CONTAINER_RUNTIME (default docker; podman works
#       rootless; KYTHIRA_COMPOSE_COMMAND overrides "<runtime> compose").
#   run_chaos.sh native OUT_DIR
#       the nodes as processes on this host, the driver on this host's
#       python3 (with boto3).
#
# KYTHIRA_DYNAMODB_GATEWAY names the dynamodb_gateway_node binary (default
# build/dynamodb_gateway_node under the checkout); it must be built with
# the RocksDB engine.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../../.." && pwd)"
usage="usage: run_chaos.sh compose|native OUT_DIR"
mode="${1:?${usage}}"
out="$(mkdir -p "${2:?${usage}}" && cd "$2" && pwd)"
gateway="${KYTHIRA_DYNAMODB_GATEWAY:-${root}/build/dynamodb_gateway_node}"
if [ ! -x "${gateway}" ]; then
    echo "no dynamodb_gateway_node at ${gateway}; set KYTHIRA_DYNAMODB_GATEWAY" >&2
    exit 2
fi
write_seconds="${WRITE_SECONDS:-100}"
rounds="${ROUNDS:-2}"
rm -f "${out}"/*.txt "${out}"/*.log

# Each kill: SIGKILL, down long enough for the survivors to elect new
# leaders and carry on, then started again on its own data.
down_seconds=6
up_seconds=8

if [ "${mode}" = compose ]; then
    runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
    read -r -a compose <<< "${KYTHIRA_COMPOSE_COMMAND:-${runtime} compose}"
    image_dir="${root}/services/dynamodb/docker/dynamodb_gateway_node"
    mkdir -p "${image_dir}/dist"
    cp "${gateway}" "${image_dir}/dist/dynamodb_gateway_node"
    "${runtime}" build -t kythira-dynamodb-node:dev "${image_dir}"
    export CHAOS_OUT="${out}"
    cd "${here}"
    trap '"${compose[@]}" logs ddb-1 ddb-2 ddb-3 > "${out}/nodes.log" 2>&1 || true;
          "${compose[@]}" down -v --remove-orphans' EXIT
    "${compose[@]}" up -d ddb-1 ddb-2 ddb-3 driver
    driver() { "${compose[@]}" exec -T driver python3 /tools/chaos_driver.py "$@"; }
    kill_node() { "${compose[@]}" kill -s KILL "ddb-$1"; }
    start_node() { "${compose[@]}" start "ddb-$1"; }
    # The driver installs boto3 when it starts; wait for it.
    for _ in $(seq 1 120); do
        if "${compose[@]}" exec -T driver python3 -c "import boto3" > /dev/null 2>&1; then
            break
        fi
        sleep 1
    done
    driver_out=/out
elif [ "${mode}" = native ]; then
    data="${out}/data"
    rm -rf "${data}"
    peers=""
    for n in 1 2 3; do
        peers="${peers}${peers:+,}${n}=http://127.0.0.1:$((7760 + n))"
    done
    pids=(0 0 0 0)
    start_node() {
        local n="$1" gws=""
        for m in 1 2 3; do
            if [ "${m}" != "${n}" ]; then
                gws="${gws}${gws:+,}${m}=127.0.0.1:$((8760 + m))"
            fi
        done
        mkdir -p "${data}/${n}"
        KYTHIRA_DYNAMODB_BIND=127.0.0.1 KYTHIRA_DYNAMODB_PORT=$((8760 + n)) \
            KYTHIRA_DYNAMODB_RAFT_BIND=127.0.0.1 KYTHIRA_DYNAMODB_RAFT_PORT=$((7760 + n)) \
            KYTHIRA_DYNAMODB_NODE_ID="${n}" KYTHIRA_DYNAMODB_PEERS="${peers}" \
            KYTHIRA_DYNAMODB_PEER_GATEWAYS="${gws}" \
            KYTHIRA_DYNAMODB_FORWARD_SECRET=dynamodb-chaos-forward-secret \
            KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT=1 KYTHIRA_DYNAMODB_ENGINE=rocksdb \
            KYTHIRA_DYNAMODB_DATA_DIR="${data}/${n}" KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES=0 \
            KYTHIRA_DYNAMODB_LOG_LEVEL=warning KYTHIRA_DYNAMODB_COMPAT=throttling=off \
            KYTHIRA_DYNAMODB_SPLIT_BYTES=524288 KYTHIRA_DYNAMODB_SPLIT_KEYS=0 \
            KYTHIRA_DYNAMODB_SPLIT_INTERVAL_MS=2000 \
            KYTHIRA_DYNAMODB_STATIC_KEYS=CHAOS-driver:chaos-secret-not-a-credential:000000000000:driver:admin \
            "${gateway}" >> "${out}/node-${n}.log" 2>&1 &
        pids[n]=$!
    }
    kill_node() { kill -KILL "${pids[$1]}" 2> /dev/null || true; wait "${pids[$1]}" 2> /dev/null || true; }
    trap 'for n in 1 2 3; do kill_node "${n}"; done' EXIT
    for n in 1 2 3; do start_node "${n}"; done
    export ENDPOINTS="http://127.0.0.1:8761,http://127.0.0.1:8762,http://127.0.0.1:8763"
    driver() { python3 "${here}/chaos_driver.py" "$@"; }
    driver_out="${out}"
else
    echo "unknown mode ${mode}" >&2
    exit 2
fi

driver wait --seconds 120
driver setup
driver write --seconds "${write_seconds}" --out "${driver_out}" > "${out}/write.log" 2>&1 &
writer=$!
# Let the first groups fill and split before the first kill.
sleep 15
for round in $(seq 1 "${rounds}"); do
    for n in 1 2 3; do
        echo "round ${round}: SIGKILL node ${n}"
        kill_node "${n}"
        sleep "${down_seconds}"
        start_node "${n}"
        sleep "${up_seconds}"
    done
done
status=0
wait "${writer}" || status=$?
cat "${out}/write.log"
if [ "${status}" != 0 ]; then
    echo "the writers failed" >&2
    exit 1
fi
driver verify --out "${driver_out}"
