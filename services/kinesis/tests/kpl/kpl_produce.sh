#!/bin/bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# KPL in the Kinesis container scenario (task 17). KPL speaks HTTPS only,
# and the scenario's nodes serve plaintext, so a local socat TLS front with
# a throwaway self-signed certificate forwards to the node; KPL is told not
# to verify it. Signatures cover the Host header KPL sends, which socat
# passes through untouched, so the node checks the real thing.
#
# Usage: kpl_produce.sh <node host:port> <stream> <count> <tag> [keys]
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: kpl_produce.sh <node host:port> <stream> <count> <tag> [keys]" >&2
    exit 2
fi
target="$1"
shift

dir="$(mktemp -d)"
openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=localhost -days 1 \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" 2>/dev/null
socat "OPENSSL-LISTEN:8443,bind=127.0.0.1,reuseaddr,fork,cert=$dir/cert.pem,key=$dir/key.pem,verify=0" \
    "TCP:$target" &
front=$!
trap 'kill "$front" 2>/dev/null || true' EXIT

tries=0
until (echo > /dev/tcp/127.0.0.1/8443) 2>/dev/null || [ "$tries" -ge 50 ]; do
    tries=$((tries + 1))
    sleep 0.1
done

/opt/kpl-producer/bin/kythira-kpl-producer https://127.0.0.1:8443 "$@"
