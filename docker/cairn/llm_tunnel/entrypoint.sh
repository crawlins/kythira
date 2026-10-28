#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Forwards 0.0.0.0:8080 here to 127.0.0.1:${LLM_REMOTE_PORT:-8080} on
# $LLM_SSH_TARGET (user@host), with the private key mounted at /run/llm-key.
# The key is copied so ssh sees mode 0600 whatever the mount gives it. The
# host key is learned on first use and kept for the container's life.

set -eu

: "${LLM_SSH_TARGET:?set CAIRN_LLM_SSH to user@host of the llama.cpp server}"
[ -s /run/llm-key ] || { echo "llm-tunnel: no key at /run/llm-key; set CAIRN_LLM_SSH_KEY" >&2; exit 2; }

install -m 0600 /run/llm-key /tmp/llm-key
exec ssh -N -T \
    -i /tmp/llm-key \
    -o BatchMode=yes \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/tmp/known_hosts \
    -L "0.0.0.0:8080:127.0.0.1:${LLM_REMOTE_PORT:-8080}" \
    "$LLM_SSH_TARGET"
