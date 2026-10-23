#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Runs raft_object_backup's create -> list -> verify -> restore-clone cycle
# against a LocalStack S3 through --endpoint
# (.kiro/specs/object-backup-s3-endpoint/ task 7, Requirement 5.4).
#
# Usage: scripts/backup-s3-localstack-cycle.sh <raft_object_backup> [endpoint]
#
# endpoint defaults to http://localhost:4566. The caller starts LocalStack
# (SERVICES=s3) with that port published and waits for it to be healthy; this
# script only talks to it. It needs curl and nothing else: LocalStack accepts
# unsigned requests, so seeding needs no AWS CLI.
#
# The seed is the same six-object node prefix as the mock full-cycle unit test
# (term, voted_for, a snapshot and three log entries), so a pass here and a
# pass there check the same thing on two stores.
#
# AWS_ENDPOINT_URL is unset first: CI points it at Oracle's build cache, and
# the tool must not be reading it anyway. The keys are LocalStack's dummies,
# so no real credential is ever sent to the local service.

set -euo pipefail

binary="${1:?usage: $0 <raft_object_backup> [endpoint]}"
endpoint="${2:-http://localhost:4566}"
bucket="kythira-backup-cycle-$$"

unset AWS_ENDPOINT_URL AWS_ENDPOINT_URL_S3 AWS_PROFILE AWS_SESSION_TOKEN
export AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test AWS_EC2_METADATA_DISABLED=true

fail() {
    echo "::error::$*" >&2
    exit 1
}

put() {
    local key="$1" body="$2"
    curl -fsS -o /dev/null -X PUT --data-binary "$body" "$endpoint/$bucket/$key" ||
        fail "could not seed $key"
}

log_key() { printf 'log/%020d' "$1"; }
entry() { printf '{"term":%d,"index":%d,"command":"","type":0}' "$1" "$2"; }

curl -fsS -o /dev/null -X PUT "$endpoint/$bucket" || fail "could not create bucket $bucket"
put node-1/term 5
put node-1/voted_for n2
put node-1/snapshot \
    '{"last_included_index":3,"last_included_term":4,"state":"aGk=","nodes":["n1","n2","n3"],"is_joint_consensus":false}'
for i in 4 5 6; do
    put "node-1/$(log_key "$i")" "$(entry 5 "$i")"
done

# Runs one verb, echoes its output, and fails the script unless it exits 0 and
# prints the expected text.
step() {
    local name="$1" expect="$2"
    shift 2
    local out status=0
    out="$("$binary" "$@" --provider s3 --endpoint "$endpoint" --region us-east-1 \
        --dest-bucket "$bucket" --dest-prefix backups 2>&1)" || status=$?
    printf '── %s (exit %d)\n%s\n' "$name" "$status" "$out"
    [ "$status" -eq 0 ] || fail "$name exited $status"
    grep -qF -- "$expect" <<<"$out" || fail "$name did not print \"$expect\""
}

step create "created b1" create --source-bucket "$bucket" --source-prefix node-1 \
    --backup-id b1 --quiesced
step list "6 objects" list
step verify "no problems" verify --backup-id b1
step restore-clone "objects written  6" restore-clone --backup-id b1 \
    --target-bucket "$bucket" --target-prefix node-1-restored

# The exit codes mean something only if the restored prefix holds the state.
for key in term voted_for "$(log_key 6)"; do
    want="$(curl -fsS "$endpoint/$bucket/node-1/$key")"
    got="$(curl -fsS "$endpoint/$bucket/node-1-restored/$key")" ||
        fail "restored prefix is missing $key"
    [ "$got" = "$want" ] || fail "restored $key differs: got '$got', want '$want'"
done
echo "backup cycle against $endpoint: OK"
