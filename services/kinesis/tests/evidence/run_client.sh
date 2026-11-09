#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Run one evidence client against a fresh recorder and write its evidence.
# Usage: run_client.sh NAME OUT.json COMMAND... ; the client gets the
# endpoint URL as its last argument. Credentials are fixed dummies, so the
# signature is real SigV4 but nothing can reach AWS.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
name=$1 out=$2
shift 2
py=${EVIDENCE_PYTHON:-python3}
tls=() scheme=http
if [[ -n ${EVIDENCE_TLS:-} ]]; then
  # A throwaway self-signed certificate; the TLS clients are told not to
  # verify it (they never leave 127.0.0.1).
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null
  tls=(--tls-cert "$tmp/cert.pem" --tls-key "$tmp/key.pem") scheme=https
fi
fifo=$(mktemp -u); mkfifo "$fifo"
extra=("${tls[@]}")
[[ -n ${EVIDENCE_STREAM_EXISTS:-} ]] && extra+=(--stream-exists)
"$py" "$here/recorder.py" --client "$name" --out "$out" "${extra[@]}" >"$fifo" &
rec=$!
read -r port <"$fifo"; rm -f "$fifo"
endpoint="$scheme://127.0.0.1:$port"
export AWS_ACCESS_KEY_ID=AKIDEXAMPLE AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
export AWS_DEFAULT_REGION=us-east-1 AWS_REGION=us-east-1
unset AWS_SESSION_TOKEN AWS_PROFILE
rc=0
"$@" "$endpoint" || rc=$?
kill -TERM "$rec"; wait "$rec" || true
exit $rc
