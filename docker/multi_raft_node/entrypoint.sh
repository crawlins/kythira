#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

# Turns the compose file's environment into `multi_raft_node`'s command line.
#
# PEERS and PEER_CONTROL are comma-separated `id:host:port`. **Service names,
# never IP addresses**: CLAUDE.md's container rules forbid static IPs because
# rootless Podman ignores `ipam.config.ipv4_address` *silently*, and a compose
# file that depends on one works under Docker and fails under Podman with no
# error to read.

NODE_ID="${NODE_ID:?NODE_ID is required}"
RAFT_PORT="${RAFT_PORT:-7000}"
DATA_PORT="${DATA_PORT:-7001}"
CONTROL_PORT="${CONTROL_PORT:-7002}"

args=(
  --node-id "${NODE_ID}"
  --bind "${BIND_ADDRESS:-0.0.0.0}"
  --raft-port "${RAFT_PORT}"
  --data-port "${DATA_PORT}"
  --control-port "${CONTROL_PORT}"
  --groups "${GROUPS:-4}"
  --key-count "${KEY_COUNT:-100000}"
  --tick-interval "${TICK_INTERVAL_MS:-2}"
  --transport "${TRANSPORT:-httplib}"
  --serializer "${SERIALIZER:-json}"
  --persistence "${PERSISTENCE:-memory}"
  --election-timeout-min "${ELECTION_TIMEOUT_MIN_MS:-150}"
  --election-timeout-max "${ELECTION_TIMEOUT_MAX_MS:-300}"
  --heartbeat-interval "${HEARTBEAT_INTERVAL_MS:-50}"
)

if [[ "${PERSISTENCE:-memory}" != "memory" ]]; then
  args+=(--data-dir "${DATA_DIR:-/var/lib/multi_raft_node}")
  mkdir -p "${DATA_DIR:-/var/lib/multi_raft_node}"
fi

# This host's own entry first: the transport's URL map is used for every
# target including self, and the binary refuses to start without it rather
# than failing later in a way that looks like a network problem.
args+=(--peer "${NODE_ID}=http://${HOSTNAME}:${RAFT_PORT}")

if [[ -n "${PEERS:-}" ]]; then
  IFS=',' read -ra entries <<< "${PEERS}"
  for entry in "${entries[@]}"; do
    id="${entry%%:*}"
    rest="${entry#*:}"
    host="${rest%%:*}"
    port="${rest##*:}"
    args+=(--peer "${id}=http://${host}:${port}")
  done
fi

# Only the inter-node network probe uses these, and only Tier E needs it.
# Absent, a peer is reported with null figures rather than guessed at.
if [[ -n "${PEER_CONTROL:-}" ]]; then
  IFS=',' read -ra entries <<< "${PEER_CONTROL}"
  for entry in "${entries[@]}"; do
    id="${entry%%:*}"
    rest="${entry#*:}"
    args+=(--peer-control "${id}=${rest}")
  done
fi

if [[ -n "${VOTERS:-}" ]]; then
  args+=(--voters "${VOTERS}")
fi

exec /usr/local/bin/multi_raft_node "${args[@]}"
