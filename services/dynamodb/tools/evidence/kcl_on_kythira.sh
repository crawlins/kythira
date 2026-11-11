#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Run KCL 2.x and 3.x on Kythira's DynamoDB API
# (.kiro/specs/dynamodb-compatible-api/ tasks 7 and 11): the evidence
# clients' KCL workers keep their lease table (and, on KCL 3.x, its
# LeaseOwnerToLeaseKeyIndex GSI and the coordinator and worker-metrics
# entries) on Kythira, while the recording stub stands in for the Kinesis
# stream. check_kcl_run.py then reads what Kythira answered.
#
# Usage:
#   kcl_on_kythira.sh compose OUT_DIR   a three-node cluster and the clients
#                                       in containers (compose.yml, profile
#                                       kythira), under
#                                       $KYTHIRA_CONTAINER_RUNTIME (default
#                                       docker; podman works rootless).
#   kcl_on_kythira.sh native OUT_DIR    one node and the clients on this
#                                       host (mvn and java 17+).
#
# KYTHIRA_DYNAMODB_GATEWAY names the dynamodb_gateway_node binary (default
# build/dynamodb_gateway_node under the checkout).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../../.." && pwd)"
usage="usage: kcl_on_kythira.sh compose|native OUT_DIR"
mode="${1:?${usage}}"
out="$(mkdir -p "${2:?${usage}}" && cd "$2" && pwd)"
rm -f "${out}"/*.jsonl
gateway="${KYTHIRA_DYNAMODB_GATEWAY:-${root}/build/dynamodb_gateway_node}"
if [ ! -x "${gateway}" ]; then
    echo "no dynamodb_gateway_node at ${gateway}; set KYTHIRA_DYNAMODB_GATEWAY" >&2
    exit 2
fi
clients=(kcl2 kcl3)
secret=evidence-secret-not-a-credential

if [ "${mode}" = compose ]; then
    runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
    read -r -a compose <<< "${KYTHIRA_COMPOSE_COMMAND:-${runtime} compose}"
    image_dir="${root}/services/dynamodb/docker/dynamodb_gateway_node"
    mkdir -p "${image_dir}/dist"
    cp "${gateway}" "${image_dir}/dist/dynamodb_gateway_node"
    "${runtime}" build -t kythira-dynamodb-node:dev "${image_dir}"
    export EVIDENCE_OUT="${out}"
    cd "${here}"
    compose+=(--profile kythira)
    trap '"${compose[@]}" logs ddb-1 ddb-2 ddb-3 > "${out}/nodes.log" 2>&1 || true;
          "${compose[@]}" down -v --remove-orphans' EXIT
    "${compose[@]}" up -d ddb-1 ddb-2 ddb-3 forwarder
    # Ready when a request through the forwarder gets an API answer from
    # ddb-2 (its unknown probe key is refused): the stub serves and the node
    # listens. A cluster still electing is fine, since the KCL retries
    # until its CreateTable lands. Polled rather than a compose
    # healthcheck, which rootless Podman runs from systemd timers a CI
    # runner lacks (record.sh).
    probe='
import sys, urllib.error, urllib.request
r = urllib.request.Request("http://127.0.0.1:8000/", data=b"{}", headers={
    "Content-Type": "application/x-amz-json-1.0",
    "X-Amz-Target": "DynamoDB_20120810.ListTables",
    "Authorization": "AWS4-HMAC-SHA256 Credential=EVIDENCE-probe/20260101/us-east-1/dynamodb/aws4_request, SignedHeaders=host, Signature=0"})
try:
    urllib.request.urlopen(r, timeout=5)
except urllib.error.HTTPError as e:
    # Any API answer (the probe signature is refused) means the cluster serves.
    sys.exit(0 if e.code in (400, 403) and b"__type" in e.read() else 1)
'
    ready=0
    for _ in $(seq 1 90); do
        if "${compose[@]}" exec -T forwarder python3 -c "${probe}" > /dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 1
    done
    if [ "${ready}" != 1 ]; then
        "${compose[@]}" logs >&2 || true
        echo "the Kythira cluster or the forwarder did not come up" >&2
        exit 1
    fi
    for c in "${clients[@]}"; do
        "${compose[@]}" run --rm -e ENDPOINT=http://forwarder:8000 java "${c}"
    done
elif [ "${mode}" = native ]; then
    api_port="${KCL_KYTHIRA_PORT:-8766}"
    stub_port="${EVIDENCE_PORT:-8767}"
    keys=""
    for c in "${clients[@]}"; do
        keys="${keys}${keys:+,}EVIDENCE-${c}:${secret}:000000000000:${c}:admin"
    done
    KYTHIRA_DYNAMODB_BIND=127.0.0.1 KYTHIRA_DYNAMODB_PORT="${api_port}" \
        KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT=1 KYTHIRA_DYNAMODB_STATIC_KEYS="${keys}" \
        "${gateway}" > "${out}/nodes.log" 2>&1 &
    node=$!
    python3 "${here}/recording_stub.py" --port "${stub_port}" --out "${out}" \
        --dynamodb-upstream "127.0.0.1:${api_port}" &
    stub=$!
    trap 'kill ${stub} ${node}' EXIT
    sleep 2
    export ENDPOINT="http://127.0.0.1:${stub_port}"
    for c in "${clients[@]}"; do
        sh "${here}/clients/run_java.sh" "${c}"
    done
else
    echo "unknown mode ${mode}" >&2
    exit 2
fi
python3 "${here}/check_kcl_run.py" "${out}" "${clients[@]}"
