#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Re-records the expression corpus against DynamoDB Local
# (.kiro/specs/dynamodb-compatible-api/ task 5, Requirement 21.3).
#
#   tools/expr-corpus/record.sh [OUT.jsonl]
#
# Starts DynamoDB Local in a container (Docker or rootless Podman, from
# $KYTHIRA_CONTAINER_RUNTIME), runs every case of cases.py against it with
# record.py, and writes the corpus (default: tests/data/expression_corpus.jsonl
# under services/dynamodb). Nothing leaves the machine: DynamoDB Local is an
# in-memory emulator, so recording costs nothing.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-${here}/../../tests/data/expression_corpus.jsonl}"
runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
image="amazon/dynamodb-local:3.3.2"
port="${KYTHIRA_DDB_LOCAL_PORT:-$((20000 + RANDOM % 10000))}"

name="kythira-expr-corpus-$$"
"${runtime}" run -d --rm --name "${name}" -p "127.0.0.1:${port}:8000" "${image}" \
    -jar DynamoDBLocal.jar -inMemory -sharedDb >/dev/null
trap '"${runtime}" rm -f "${name}" >/dev/null 2>&1 || true' EXIT

endpoint="http://127.0.0.1:${port}"
for _ in $(seq 1 60); do
    if curl -s -o /dev/null "${endpoint}"; then
        break
    fi
    sleep 1
done

python3 "${here}/record.py" "${endpoint}" "${out}"
echo "recorded $(wc -l <"${out}") cases to ${out}"
