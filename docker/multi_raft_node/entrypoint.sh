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

# A container `docker_quorum_manager` created is told its id as
# KYTHIRA_NODE_ID; one compose started is told NODE_ID.
NODE_ID="${NODE_ID:-${KYTHIRA_NODE_ID:-}}"
NODE_ID="${NODE_ID:?NODE_ID (or KYTHIRA_NODE_ID) is required}"
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

# Elastic shard capacity (.kiro/specs/elastic-shard-capacity/ task 16). Off
# unless CAPACITY_ROLE says otherwise, so the measurement host is unchanged.
CAPACITY_ROLE="${CAPACITY_ROLE:-off}"
if [[ "${CAPACITY_ROLE}" != "off" ]]; then
  CAPACITY_PORT="${CAPACITY_PORT:-7003}"
  args+=(--capacity-role "${CAPACITY_ROLE}"
         --capacity-heartbeat "${CAPACITY_HEARTBEAT_MS:-1000}")
  if [[ -n "${SPLIT_KEYS:-}" ]]; then
    args+=(--split-keys "${SPLIT_KEYS}")
  fi
  if [[ "${CAPACITY_ROLE}" == "member" ]]; then
    args+=(--capacity-controller "${CAPACITY_CONTROLLER:?CAPACITY_CONTROLLER is required for a member}")
  fi
  if [[ "${CAPACITY_ROLE}" == "controller" ]]; then
    args+=(--capacity-port "${CAPACITY_PORT}"
           --capacity-cluster "${CAPACITY_CLUSTER:?CAPACITY_CLUSTER is required for a controller}"
           --capacity-network "${CAPACITY_NETWORK:?CAPACITY_NETWORK is required for a controller}"
           --capacity-image "${CAPACITY_IMAGE:?CAPACITY_IMAGE is required for a controller}"
           --capacity-docker-url "${CAPACITY_DOCKER_URL:-unix:///var/run/docker.sock}"
           --capacity-max-nodes "${CAPACITY_MAX_NODES:-0}"
           --capacity-shards-high "${CAPACITY_SHARDS_HIGH:-200}"
           --capacity-shards-low "${CAPACITY_SHARDS_LOW:-80}"
           --capacity-sustained "${CAPACITY_SUSTAINED_MS:-300000}"
           --capacity-dry-run "${CAPACITY_DRY_RUN:-0}")
    # What a machine this controller creates needs in order to join: this
    # cluster's shape, copied from this container's own environment, and the
    # way back here. It starts with no groups; its replicas arrive by lazy
    # creation once the controller adds it to one.
    for var in PEERS PEER_CONTROL VOTERS KEY_COUNT TICK_INTERVAL_MS TRANSPORT SERIALIZER \
               PERSISTENCE ELECTION_TIMEOUT_MIN_MS ELECTION_TIMEOUT_MAX_MS \
               HEARTBEAT_INTERVAL_MS RAFT_PORT DATA_PORT CONTROL_PORT SPLIT_KEYS \
               CAPACITY_HEARTBEAT_MS; do
      if [[ -n "${!var:-}" ]]; then
        args+=(--capacity-join-env "${var}=${!var}")
      fi
    done
    args+=(--capacity-join-env "GROUPS=0"
           --capacity-join-env "CAPACITY_ROLE=member"
           --capacity-join-env "CAPACITY_CONTROLLER=${HOSTNAME}:${CAPACITY_PORT}")
  fi
fi

exec /usr/local/bin/multi_raft_node "${args[@]}"
