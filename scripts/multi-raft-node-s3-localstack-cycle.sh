#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Runs a three-host multi_raft_node cluster on --persistence s3 against a real
# S3-compatible service: write, stop every host, restart, read every key back
# (.kiro/specs/multi-raft-node-s3-persistence/ task 9, Requirement 10.5).
#
# Usage: scripts/multi-raft-node-s3-localstack-cycle.sh <multi_raft_node> [endpoint]
#
# endpoint defaults to http://localhost:4566. The caller starts the service
# (LocalStack with SERVICES=s3, as for scripts/backup-s3-localstack-cycle.sh)
# and waits for it to be healthy. This script needs curl and nothing else.
#
# The cycle runs twice, with --s3-write-behind 0 and 1, each under its own
# bucket. The tests/ process test does the same against the in-process mock;
# this is the check that the mock and a real implementation agree on the
# conditional writes and listings the engine depends on.
#
# AWS_ENDPOINT_URL is unset first: CI points it at Oracle's build cache, and
# the binary must not read it anyway. The keys are LocalStack's dummies.

set -euo pipefail

binary="${1:?usage: $0 <multi_raft_node> [endpoint]}"
endpoint="${2:-http://localhost:4566}"
keys=40
base_port=$((20000 + $$ % 20000))
workdir="$(mktemp -d)"

unset AWS_ENDPOINT_URL AWS_ENDPOINT_URL_S3 AWS_PROFILE AWS_SESSION_TOKEN
export AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test AWS_EC2_METADATA_DISABLED=true

pids=()
cleanup() {
    for pid in "${pids[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -rf "$workdir"
}
trap cleanup EXIT

fail() {
    echo "::error::$*" >&2
    for log in "$workdir"/*.log; do
        [ -f "$log" ] || continue
        echo "── $log" >&2
        tail -40 "$log" >&2
    done
    exit 1
}

raft_port() { echo $((base_port + $1 * 10)); }
data_port() { echo $((base_port + $1 * 10 + 1)); }
control_port() { echo $((base_port + $1 * 10 + 2)); }

start_hosts() {
    local bucket="$1" write_behind="$2"
    pids=()
    for id in 1 2 3; do
        local peers=()
        for peer in 1 2 3; do
            peers+=(--peer "$peer=http://127.0.0.1:$(raft_port "$peer")")
        done
        "$binary" --node-id "$id" --bind 127.0.0.1 \
            --raft-port "$(raft_port "$id")" --data-port "$(data_port "$id")" \
            --control-port "$(control_port "$id")" --groups 2 "${peers[@]}" \
            --election-timeout-min 300 --election-timeout-max 600 --op-timeout 5000 \
            --persistence s3 --s3-bucket "$bucket" --s3-prefix ci \
            --s3-endpoint "$endpoint" --s3-region us-east-1 \
            --s3-write-behind "$write_behind" >>"$workdir/host$id.log" 2>&1 &
        pids+=($!)
    done
    for id in 1 2 3; do
        local ready=0
        for _ in $(seq 1 120); do
            if curl -fsS -o /dev/null "http://127.0.0.1:$(control_port "$id")/ready" 2>/dev/null; then
                ready=1
                break
            fi
            sleep 0.5
        done
        [ "$ready" = 1 ] || fail "host $id never became ready"
    done
}

stop_hosts() {
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    pids=()
}

# test_key_value_state_machine's command: type byte, u32 LE key length, key,
# and for a put a u32 LE value length and the value. The format strings are
# built from numbers on purpose: printf's \x escapes are how bash writes bytes.
# shellcheck disable=SC2059
u32le() { printf "\\x$(printf %02x $(($1 & 255)))\\x$(printf %02x $((($1 >> 8) & 255)))\\x00\\x00"; }
command_file() {
    local type="$1" key="$2" value="${3:-}" file="$workdir/cmd"
    {
        # shellcheck disable=SC2059
        printf "\\x0$type"
        u32le "${#key}"
        printf '%s' "$key"
        if [ "$type" = 1 ]; then
            u32le "${#value}"
            printf '%s' "$value"
        fi
    } >"$file"
    echo "$file"
}

# Posts to each host in turn until one answers 200, for up to 30 s; prints
# the body. A not-leader answer (421) just moves on to the next host.
post_any() {
    local path="$1" body_file="$2"
    for _ in $(seq 1 60); do
        for id in 1 2 3; do
            if curl -fsS --max-time 6 -X POST -H 'Content-Type: application/json' \
                --data-binary "@$body_file" "http://127.0.0.1:$(data_port "$id")$path" 2>/dev/null; then
                return 0
            fi
        done
        sleep 0.5
    done
    return 1
}

key() { printf 'k%06d' "$1"; }

cycle() {
    local write_behind="$1" bucket="kythira-node-cycle-wb$1-$$"
    curl -fsS -o /dev/null -X PUT "$endpoint/$bucket" || fail "could not create bucket $bucket"

    start_hosts "$bucket" "$write_behind"
    for i in $(seq 1 "$keys"); do
        post_any "/kv/put?key=$(key "$i")" "$(command_file 1 "$(key "$i")" "value-$i")" \
            >/dev/null || fail "write-behind $write_behind: put $(key "$i") never acknowledged"
    done
    stop_hosts

    # Requirement 2.3: each host under its own node prefix.
    local listing
    listing="$(curl -fsS "$endpoint/$bucket?list-type=2&prefix=ci/nodes/")"
    for id in 1 2 3; do
        grep -q "<Key>ci/nodes/$id/groups/" <<<"$listing" ||
            fail "write-behind $write_behind: nothing under ci/nodes/$id/"
    done

    start_hosts "$bucket" "$write_behind"
    for i in $(seq 1 "$keys"); do
        local got
        got="$(post_any "/kv/get?key=$(key "$i")&read=log" "$(command_file 0 "$(key "$i")")")" ||
            fail "write-behind $write_behind: get $(key "$i") failed after restart"
        [ "$got" = "value-$i" ] ||
            fail "write-behind $write_behind: $(key "$i") read back as '$got', want 'value-$i'"
    done
    stop_hosts
    echo "multi_raft_node on $endpoint, write-behind $write_behind: $keys keys survived a restart"
}

cycle 0
cycle 1
