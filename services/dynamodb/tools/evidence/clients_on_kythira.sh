#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Run every conformance client on Kythira's DynamoDB API
# (.kiro/specs/dynamodb-compatible-api/ tasks 7, 11, 16 and 24), each
# through the recording stub, which passes its DynamoDB and Streams calls
# to Kythira and records what Kythira answered:
#
#   * the AWS CLI, boto3, aws-sdk-java-v2 and the DynamoDB lock client run
#     their evidence flows (task 1), each failing on an error it does not
#     expect;
#   * Terraform applies an aws_dynamodb_table with indexes, TTL, a stream
#     and tags, plans again to find nothing to change, changes it in place
#     and destroys it (clients/terraform);
#   * KCL 2.x and 3.x keep their lease table (and, on KCL 3.x, its
#     LeaseOwnerToLeaseKeyIndex GSI and the coordinator and worker-metrics
#     entries) on Kythira, while the stub stands in for the Kinesis stream;
#   * the Streams Kinesis Adapter (Requirement 14.5) consumes a real
#     Kythira stream: its client writes to a table before and while its KCL
#     3.x worker runs, and fails unless the worker sees every item exactly
#     once.
#
# check_client_runs.py then reads what Kythira answered each client.
#
# Usage:
#   clients_on_kythira.sh compose OUT_DIR [CLIENT...]
#       a three-node cluster and the clients in containers (compose.yml,
#       profile kythira), under $KYTHIRA_CONTAINER_RUNTIME (default docker;
#       podman works rootless).
#   clients_on_kythira.sh native OUT_DIR [CLIENT...]
#       one node and the Java and Python clients on this host (mvn, java
#       17+, and boto3 for boto3); the AWS CLI and Terraform need theirs.
#
# CLIENT names a subset (default all): aws-cli boto3 java-sdk lock-client
# terraform kcl2 kcl3 streams-adapter.
#
# KYTHIRA_DYNAMODB_GATEWAY names the dynamodb_gateway_node binary (default
# build/dynamodb_gateway_node under the checkout).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../../../.." && pwd)"
usage="usage: clients_on_kythira.sh compose|native OUT_DIR [CLIENT...]"
mode="${1:?${usage}}"
out="$(mkdir -p "${2:?${usage}}" && cd "$2" && pwd)"
shift 2
rm -f "${out}"/*.jsonl
gateway="${KYTHIRA_DYNAMODB_GATEWAY:-${root}/build/dynamodb_gateway_node}"
if [ ! -x "${gateway}" ]; then
    echo "no dynamodb_gateway_node at ${gateway}; set KYTHIRA_DYNAMODB_GATEWAY" >&2
    exit 2
fi
clients=(aws-cli boto3 java-sdk lock-client terraform kcl2 kcl3 streams-adapter)
if [ "$#" -gt 0 ]; then
    clients=("$@")
fi
# The table the Streams Kinesis Adapter's client creates and reads.
source_table=adapter-source
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
        case "${c}" in
            aws-cli | boto3 | terraform)
                "${compose[@]}" run --rm -e ENDPOINT=http://forwarder:8000 "${c}" ;;
            *)
                "${compose[@]}" run --rm -e ENDPOINT=http://forwarder:8000 \
                    -e SOURCE_TABLE="${source_table}" java "${c}" ;;
        esac
        # Keep the nodes' logs after every client: a later `run` can
        # recreate the nodes (it did before terraform under rootless
        # Podman), and their logs, with the cause of any 500 an earlier
        # client met, go with the old containers.
        "${compose[@]}" logs ddb-1 ddb-2 ddb-3 > "${out}/nodes-after-${c}.log" 2>&1 || true
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
    export SOURCE_TABLE="${source_table}"
    for c in "${clients[@]}"; do
        case "${c}" in
            aws-cli) sh "${here}/clients/aws_cli.sh" ;;
            boto3) python3 "${here}/clients/boto3_client.py" ;;
            terraform) sh "${here}/clients/terraform/run_terraform.sh" ;;
            *) sh "${here}/clients/run_java.sh" "${c}" ;;
        esac
    done
else
    echo "unknown mode ${mode}" >&2
    exit 2
fi
status=0
python3 "${here}/check_client_runs.py" "${out}" "${clients[@]}" || status=$?
if [ "${status}" -ne 0 ]; then
    # The artifact keeps nodes.log, but say here why any request was
    # answered 500 (the gateway logs each cause), so the job log alone
    # names it.
    if [ "${mode}" = compose ]; then
        "${compose[@]}" logs ddb-1 ddb-2 ddb-3 > "${out}/nodes.log" 2>&1 || true
    fi
    grep -h "answered InternalServerError" "${out}"/nodes*.log 2>/dev/null | sort -u >&2 || true
fi
exit "${status}"
