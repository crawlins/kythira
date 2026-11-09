#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Record the DynamoDB wire evidence (.kiro/specs/dynamodb-compatible-api/
# task 1): run every client against the recording stub and summarize.
#
# Usage:
#   record.sh compose OUT_DIR   clients in containers (compose.yml), under
#                               $KYTHIRA_CONTAINER_RUNTIME (default docker;
#                               podman works rootless). What CI runs.
#   record.sh native OUT_DIR    clients on this host: python3 with boto3,
#                               AWS CLI v2 on PATH (skipped if absent), mvn
#                               and java 17+. For a sandbox whose
#                               containers cannot reach Maven Central.
#
# Then `summarize.py OUT_DIR` writes OUT_DIR/summary.json and README.md.
# To refresh the checked-in evidence, record into services/dynamodb/evidence/.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:?usage: record.sh compose|native OUT_DIR}"
out="$(mkdir -p "${2:?usage: record.sh compose|native OUT_DIR}" && cd "$2" && pwd)"
rm -f "${out}"/*.jsonl
java_clients=(java-sdk lock-client kcl2 kcl3 streams-adapter)

if [ "${mode}" = compose ]; then
    runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
    read -r -a compose <<< "${KYTHIRA_COMPOSE_COMMAND:-${runtime} compose}"
    export EVIDENCE_OUT="${out}"
    cd "${here}"
    trap '"${compose[@]}" down -v --remove-orphans' EXIT
    # Readiness is polled here rather than through a compose healthcheck:
    # rootless Podman runs healthchecks from systemd timers, which a CI
    # runner without a user session does not have, so `up --wait` reports
    # the stub unhealthy even when it is serving.
    "${compose[@]}" up -d stub
    ready=0
    for _ in $(seq 1 60); do
        if "${compose[@]}" exec -T stub python3 -c \
            "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/')" \
            > /dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 1
    done
    if [ "${ready}" != 1 ]; then
        "${compose[@]}" logs stub >&2 || true
        echo "recording stub did not come up" >&2
        exit 1
    fi
    "${compose[@]}" run --rm aws-cli
    "${compose[@]}" run --rm boto3
    for c in "${java_clients[@]}"; do
        "${compose[@]}" run --rm java "${c}"
    done
elif [ "${mode}" = native ]; then
    port="${EVIDENCE_PORT:-8765}"
    python3 "${here}/recording_stub.py" --port "${port}" --out "${out}" &
    stub=$!
    trap 'kill ${stub}' EXIT
    sleep 1
    export ENDPOINT="http://127.0.0.1:${port}"
    if command -v aws > /dev/null && aws --version 2>&1 | grep -q '^aws-cli/2'; then
        sh "${here}/clients/aws_cli.sh"
    else
        echo "warning: no AWS CLI v2 on PATH; aws-cli.jsonl not recorded" >&2
    fi
    python3 "${here}/clients/boto3_client.py"
    for c in "${java_clients[@]}"; do
        sh "${here}/clients/run_java.sh" "${c}"
    done
else
    echo "unknown mode ${mode}" >&2
    exit 2
fi
python3 "${here}/summarize.py" "${out}"
echo "evidence recorded in ${out}"
