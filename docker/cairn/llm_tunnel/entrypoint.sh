#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Forwards 127.0.0.1:8081 here to 127.0.0.1:${LLM_REMOTE_PORT:-8080} on
# $LLM_SSH_TARGET (user@host), with the private key mounted at /run/llm-key,
# and serves 0.0.0.0:8080 with json_repair.py in front of it, which unwraps
# final answers whose JSON a reasoning block or fence surrounds (JSON_REPAIR
# =off turns that off). The key is copied so ssh sees mode 0600 whatever the
# mount gives it. The host key is learned on first use and kept for the
# container's life. If either process exits, the other is stopped and the
# container exits with it, so a dead tunnel is never hidden behind a proxy
# that answers 502.

set -eu

: "${LLM_SSH_TARGET:?set CAIRN_LLM_SSH to user@host of the llama.cpp server}"
[ -s /run/llm-key ] || { echo "llm-tunnel: no key at /run/llm-key; set CAIRN_LLM_SSH_KEY" >&2; exit 2; }

install -m 0600 /run/llm-key /tmp/llm-key
ssh -N -T \
    -i /tmp/llm-key \
    -o BatchMode=yes \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/tmp/known_hosts \
    -L "127.0.0.1:8081:127.0.0.1:${LLM_REMOTE_PORT:-8080}" \
    "$LLM_SSH_TARGET" &
ssh_pid=$!
JSON_REPAIR_PORT=8080 JSON_REPAIR_UPSTREAM=http://127.0.0.1:8081 \
    python3 -u /usr/local/lib/llm-tunnel/json_repair.py &
repair_pid=$!

trap 'kill "$ssh_pid" "$repair_pid" 2>/dev/null' TERM INT
while kill -0 "$ssh_pid" 2>/dev/null && kill -0 "$repair_pid" 2>/dev/null; do
    sleep 2
done
echo "llm-tunnel: ssh or json_repair exited; stopping" >&2
kill "$ssh_pid" "$repair_pid" 2>/dev/null || true
exit 1
