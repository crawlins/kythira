#!/usr/bin/env bash
set -euo pipefail

# Create data directory if it does not exist
DATA_DIR="${DATA_DIR:-/var/lib/chaos_node}"
mkdir -p "$DATA_DIR"

# Create a dedicated iptables chain for the harness so it never needs to touch
# the INPUT chain directly. Idempotent — "Chain already exists" is not an error.
iptables -N CHAOS 2>/dev/null || true
iptables -C INPUT -j CHAOS 2>/dev/null || iptables -A INPUT -j CHAOS

# Hand off to chaos_node as PID 1 so signals are delivered correctly.
exec chaos_node "$@"
