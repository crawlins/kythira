#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Take one Tier C row: N `multi_raft_node` processes on this machine, and
# `multi_raft_bench` in a process of its own.
# `.kiro/specs/multi-raft-host-binary/` task 6.
#
# **Tier C is one host process per node.** That is the whole difference from
# Tier B, and it is what `.kiro/specs/multi-raft-performance/` Requirement 3.3
# needs before a like-for-like comparison against an external number is
# permitted at all — for a no-fsync number, which is the first time anything in
# this project has qualified.
#
# The driver is a separate process deliberately (Requirement 3.1): sharing one
# with a host makes the host's CPU and the client's indistinguishable in every
# number.

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run-tier-c-row.sh [options]

  --build-dir DIR        Where multi_raft_node and multi_raft_bench live
                         (default build-default).
  --nodes N              Host processes to start (default 3).
  --groups N             Shards, pre-split (default 4).
  --operations N         Measured operations per repetition (default 400).
  --in-flight N          Concurrency (default 16).
  --value-bytes N        (default 128)
  --repetitions N        (default 5)
  --scenario NAME        write | read-state | read-log | read-local (write).
  --tick-interval MS     (default 2)
  --transport NAME       httplib | beast (default httplib)
  --persistence MODE     memory | file-buffered | file-barrier (default memory)
  --data-threads N       Threads serving each host's data path. A handler
                         blocks for the whole commit, so this caps client
                         operations in flight per host. 0 keeps cpp-httplib's
                         default of max(8, cores-1).
  --base-port P          First port; each host takes three (default 19000).
  --out-dir DIR          Where the driver writes its CSV/JSON (test_results).
  --keep                 Leave the hosts running after the row.
  --help
USAGE
}

BUILD_DIR="build-default"
NODES=3
# NOT `GROUPS`. `GROUPS` is a Bash special variable — it holds the invoking
# user's group-ID array — and assigning to it sets element 0 while `${GROUPS}`
# keeps expanding to the real primary GID. This script spent a debugging session
# launching hosts with `--groups 1000` (the developer's GID) after being asked
# for two, and nothing anywhere reported an error: the hosts dutifully created a
# thousand shards, elected in a thousand groups on four cores, and never all
# reached a leader.
GROUP_COUNT=4
OPERATIONS=400
IN_FLIGHT=16
VALUE_BYTES=128
REPETITIONS=5
SCENARIO="write"
TICK_INTERVAL=2
TRANSPORT="httplib"
PERSISTENCE="memory"
DATA_THREADS=0
BASE_PORT=19000
OUT_DIR="test_results"
KEEP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --nodes) NODES="$2"; shift 2 ;;
        --groups) GROUP_COUNT="$2"; shift 2 ;;
        --operations) OPERATIONS="$2"; shift 2 ;;
        --in-flight) IN_FLIGHT="$2"; shift 2 ;;
        --value-bytes) VALUE_BYTES="$2"; shift 2 ;;
        --repetitions) REPETITIONS="$2"; shift 2 ;;
        --scenario) SCENARIO="$2"; shift 2 ;;
        --tick-interval) TICK_INTERVAL="$2"; shift 2 ;;
        --transport) TRANSPORT="$2"; shift 2 ;;
        --persistence) PERSISTENCE="$2"; shift 2 ;;
        --data-threads) DATA_THREADS="$2"; shift 2 ;;
        --base-port) BASE_PORT="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "run-tier-c-row.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

NODE_BIN="${BUILD_DIR}/multi_raft_node"
BENCH_BIN="${BUILD_DIR}/multi_raft_bench"
for bin in "${NODE_BIN}" "${BENCH_BIN}"; do
    if [[ ! -x "${bin}" ]]; then
        echo "run-tier-c-row.sh: ${bin} is not built. cmake --build ${BUILD_DIR} --target multi_raft_node multi_raft_bench" >&2
        exit 2
    fi
done

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kythira-tier-c-XXXXXX")"
PIDS=()

# An EXIT trap rather than a trailing block, so the failure paths are covered
# too — `.kiro/specs/multi-raft-performance/` learned that one on a cloud
# instance that was killed by hand mid-run and torn down anyway.
cleanup() {
    if [[ ${KEEP} -eq 1 ]]; then
        echo "run-tier-c-row.sh: --keep: hosts left running, work dir ${WORK_DIR}"
        return
    fi
    for pid in "${PIDS[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${PIDS[@]:-}"; do
        if [[ -n "${pid}" ]]; then
            wait "${pid}" 2>/dev/null || true
        fi
    done
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

port_of() { echo $(( BASE_PORT + ($1 - 1) * 3 + $2 )); }

PEER_ARGS=()
PEER_CONTROL_ARGS=()
HOST_ARGS=()
for (( id = 1; id <= NODES; ++id )); do
    PEER_ARGS+=(--peer "${id}=http://127.0.0.1:$(port_of "${id}" 0)")
    PEER_CONTROL_ARGS+=(--peer-control "${id}=127.0.0.1:$(port_of "${id}" 2)")
    HOST_ARGS+=(--host "${id}@127.0.0.1:$(port_of "${id}" 1):$(port_of "${id}" 2)")
done

VOTERS="$(seq -s, 1 "${NODES}")"

for (( id = 1; id <= NODES; ++id )); do
    args=(
        --node-id "${id}"
        --bind 127.0.0.1
        --raft-port "$(port_of "${id}" 0)"
        --data-port "$(port_of "${id}" 1)"
        --control-port "$(port_of "${id}" 2)"
        --voters "${VOTERS}"
        --groups "${GROUP_COUNT}"
        --tick-interval "${TICK_INTERVAL}"
        --transport "${TRANSPORT}"
        --persistence "${PERSISTENCE}"
        --data-threads "${DATA_THREADS}"
        "${PEER_ARGS[@]}"
        "${PEER_CONTROL_ARGS[@]}"
    )
    if [[ "${PERSISTENCE}" != "memory" ]]; then
        args+=(--data-dir "${WORK_DIR}/node-${id}")
    fi
    "${NODE_BIN}" "${args[@]}" > "${WORK_DIR}/node-${id}.log" 2>&1 &
    PIDS+=($!)
done

echo "run-tier-c-row.sh: ${NODES} host processes started; logs under ${WORK_DIR}"

# Requirement 5.4's network figures, taken BEFORE the measured window. On one
# machine they describe loopback and are near-meaningless as a network
# measurement — which is exactly why they are recorded rather than assumed: a
# Tier C row and a Tier E row differ in this and a reader should be able to see
# by how much.
sleep 1
for (( id = 1; id <= NODES; ++id )); do
    if command -v curl >/dev/null 2>&1; then
        echo "  network probe from node ${id}: $(curl -sf --max-time 60 "http://127.0.0.1:$(port_of "${id}" 2)/probe" || echo 'unavailable')"
    fi
done

mkdir -p "${OUT_DIR}"
"${BENCH_BIN}" \
    "${HOST_ARGS[@]}" \
    --groups "${GROUP_COUNT}" \
    --key-space 100000 \
    --tier c \
    --placement "all host processes and the driver on $(hostname), loopback" \
    --transport "${TRANSPORT}" \
    --durability "${PERSISTENCE}" \
    --tick-interval "${TICK_INTERVAL}" \
    --operations "${OPERATIONS}" \
    --in-flight "${IN_FLIGHT}" \
    --value-bytes "${VALUE_BYTES}" \
    --repetitions "${REPETITIONS}" \
    --scenario "${SCENARIO}" \
    --axis tier-c \
    --out-dir "${OUT_DIR}"
