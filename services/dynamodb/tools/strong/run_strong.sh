#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Multi-region strong consistency end to end (.kiro/specs/
# dynamodb-compatible-api/ task 27.8): three regions, each a one-node
# dynamodb_gateway_node cluster on RocksDB, whose strong hosts are the
# voters of one set of groups.
#
# strong_driver.py makes a table strong in all three regions, checks that
# a write in one region is read strongly in every other and that racing
# conditional writes have one winner each, then this script:
#   1. SIGKILLs us-west-2 while the other two keep writing;
#   2. starts it again on its own data and checks that it reads every
#      write made while it was down, and writes again;
#   3. SIGKILLs the other two, and checks that us-east-1 alone refuses
#      writes but still answers eventually consistent reads;
#   4. starts them again and checks every region reads every
#      acknowledged write.
#
# Usage:
#   run_strong.sh compose OUT_DIR
#       the regions and the driver in containers (compose.yml), under
#       $KYTHIRA_CONTAINER_RUNTIME (default docker; podman works
#       rootless; KYTHIRA_COMPOSE_COMMAND overrides "<runtime> compose").
#   run_strong.sh native OUT_DIR
#       the regions as processes on this host, the driver on this host's
#       python3 (with boto3).
#
# KYTHIRA_DYNAMODB_GATEWAY names the dynamodb_gateway_node binary (default
# build/dynamodb_gateway_node under the checkout); it must be built with
# the RocksDB engine.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../../.." && pwd)"
usage="usage: run_strong.sh compose|native OUT_DIR"
mode="${1:?${usage}}"
out="$(mkdir -p "${2:?${usage}}" && cd "$2" && pwd)"
gateway="${KYTHIRA_DYNAMODB_GATEWAY:-${root}/build/dynamodb_gateway_node}"
if [ ! -x "${gateway}" ]; then
    echo "no dynamodb_gateway_node at ${gateway}; set KYTHIRA_DYNAMODB_GATEWAY" >&2
    exit 2
fi
rm -f "${out}"/*.txt "${out}"/*.log

regions=(us-east-1 us-east-2 us-west-2)
services=(east1 east2 west2)

if [ "${mode}" = compose ]; then
    runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
    read -r -a compose <<< "${KYTHIRA_COMPOSE_COMMAND:-${runtime} compose}"
    image_dir="${root}/services/dynamodb/docker/dynamodb_gateway_node"
    mkdir -p "${image_dir}/dist"
    cp "${gateway}" "${image_dir}/dist/dynamodb_gateway_node"
    "${runtime}" build -t kythira-dynamodb-node:dev "${image_dir}"
    export STRONG_OUT="${out}"
    cd "${here}"
    trap '"${compose[@]}" logs "${services[@]}" > "${out}/nodes.log" 2>&1 || true;
          "${compose[@]}" down -v --remove-orphans' EXIT
    "${compose[@]}" up -d "${services[@]}" driver
    driver() { "${compose[@]}" exec -T driver python3 /tools/strong_driver.py "$@"; }
    kill_region() { "${compose[@]}" kill -s KILL "${services[$1]}"; }
    start_region() { "${compose[@]}" start "${services[$1]}"; }
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
    strong_peers=""
    for i in 0 1 2; do
        strong_peers="${strong_peers}${strong_peers:+,}$((10 * i + 11))=${regions[$i]}@http://127.0.0.1:$((7790 + i))"
    done
    pids=(0 0 0)
    start_region() {
        local i="$1" gws=""
        for j in 0 1 2; do
            if [ "${j}" != "${i}" ]; then
                gws="${gws}${gws:+,}$((10 * j + 11))=127.0.0.1:$((8770 + j))"
            fi
        done
        mkdir -p "${data}/${i}"
        KYTHIRA_DYNAMODB_BIND=127.0.0.1 KYTHIRA_DYNAMODB_PORT=$((8770 + i)) \
            KYTHIRA_DYNAMODB_REGION="${regions[$i]}" \
            KYTHIRA_DYNAMODB_RAFT_BIND=127.0.0.1 KYTHIRA_DYNAMODB_RAFT_PORT=$((7770 + i)) \
            KYTHIRA_DYNAMODB_NODE_ID=$((i + 1)) \
            KYTHIRA_DYNAMODB_PEERS="$((i + 1))=http://127.0.0.1:$((7770 + i))" \
            KYTHIRA_DYNAMODB_FORWARD_SECRET=dynamodb-strong-forward-secret \
            KYTHIRA_DYNAMODB_STRONG_NODE_ID=$((10 * i + 11)) \
            KYTHIRA_DYNAMODB_STRONG_PEERS="${strong_peers}" \
            KYTHIRA_DYNAMODB_STRONG_RAFT_BIND=127.0.0.1 \
            KYTHIRA_DYNAMODB_STRONG_RAFT_PORT=$((7790 + i)) \
            KYTHIRA_DYNAMODB_STRONG_GATEWAYS="${gws}" \
            KYTHIRA_DYNAMODB_STRONG_SECRET=dynamodb-strong-secret-shared-by-regions \
            KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS=100 \
            KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS=1000 \
            KYTHIRA_DYNAMODB_STRONG_ELECTION_MAX_MS=2000 \
            KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT=1 KYTHIRA_DYNAMODB_ENGINE=rocksdb \
            KYTHIRA_DYNAMODB_DATA_DIR="${data}/${i}" KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES=0 \
            KYTHIRA_DYNAMODB_LOG_LEVEL=warning KYTHIRA_DYNAMODB_COMPAT=throttling=off \
            KYTHIRA_DYNAMODB_STATIC_KEYS=STRONG-driver:strong-secret-not-a-credential:000000000000:driver:admin \
            "${gateway}" >> "${out}/${services[$i]}.log" 2>&1 &
        pids[i]=$!
    }
    kill_region() { kill -KILL "${pids[$1]}" 2> /dev/null || true; wait "${pids[$1]}" 2> /dev/null || true; }
    trap 'for i in 0 1 2; do kill_region "${i}"; done' EXIT
    for i in 0 1 2; do start_region "${i}"; done
    export ENDPOINTS="us-east-1=http://127.0.0.1:8770,us-east-2=http://127.0.0.1:8771,us-west-2=http://127.0.0.1:8772"
    driver() { python3 "${here}/strong_driver.py" "$@"; }
    driver_out="${out}"
else
    echo "unknown mode ${mode}" >&2
    exit 2
fi

driver wait --seconds 120
driver setup
driver check
driver write --prefix base --count 30 --out "${driver_out}" "${regions[@]}"

echo "SIGKILL us-west-2"
kill_region 2
driver write --prefix down --count 30 --out "${driver_out}" us-east-1 us-east-2
driver verify --prefix down --out "${driver_out}" us-east-2

echo "start us-west-2 on its own data"
start_region 2
driver verify --prefix down --out "${driver_out}" us-west-2
driver write --prefix back --count 30 --out "${driver_out}" us-west-2
driver verify --prefix back --out "${driver_out}" us-east-1

echo "SIGKILL us-east-2 and us-west-2"
kill_region 1
kill_region 2
driver refused us-east-1

echo "start us-east-2 and us-west-2"
start_region 1
start_region 2
for region in "${regions[@]}"; do
    driver verify --prefix base --out "${driver_out}" "${region}"
done
echo "strong consistency scenario passed"
