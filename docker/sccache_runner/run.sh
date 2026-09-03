#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The CI-job half of the sccache acceptance tests
# (.kiro/specs/redis-compatible-kv/ Requirement 17): build the probe crate
# KYTHIRA_BUILDS times (default 2) from a clean target directory each time,
# printing one machine-readable line per build for the scenario test to parse.
#
# Every SCCACHE_* variable is passed straight through from the compose
# environment; this script sets none of them, so what sccache does is what a
# real runner with the same environment would do.
#
# One thing here is deliberate and the operator documentation depends on it.
# sccache 0.10's server probes its storage at startup (`Storage::check` in
# src/cache/cache.rs): a write that is refused only demotes it to read-only
# mode, but a read that fails — the gateway down, or a key the user may not
# see — aborts the server, and every subsequent `sccache rustc` fails with
# "Server startup failed". Left alone, that makes an unreachable cache a
# *build dependency*. What makes it an accelerant instead is the four lines
# below: start the server explicitly, and only export RUSTC_WRAPPER if that
# worked. A CI job that wants Requirement 17.2's property has to do the same.

set -euo pipefail

export CARGO_INCREMENTAL=0          # sccache never caches incremental builds
export SCCACHE_ERROR_LOG="${SCCACHE_ERROR_LOG:-/tmp/sccache-server.log}"
export SCCACHE_LOG="${SCCACHE_LOG:-info}"

builds="${KYTHIRA_BUILDS:-2}"
mode=uncached
if sccache --start-server >/tmp/sccache-start.log 2>&1; then
    mode=cached
    export RUSTC_WRAPPER=sccache
else
    echo "sccache server refused to start; building without a cache:"
    sed 's/^/    /' /tmp/sccache-start.log
fi
echo "KYTHIRA_MODE mode=${mode}"

cd /work/crate
if [[ -n "${KYTHIRA_SALT:-}" ]]; then
    # A fresh salt is a fresh cache key; see the SALT comment in lib.rs.
    sed -i "s|^pub const SALT: u64 = .*|pub const SALT: u64 = ${KYTHIRA_SALT}; // KYTHIRA_SALT|" src/lib.rs
    grep -q "SALT: u64 = ${KYTHIRA_SALT};" src/lib.rs
fi
ok=0
for ((i = 1; i <= builds; i++)); do
    rm -rf target
    if [[ "${mode}" == cached ]]; then
        sccache --zero-stats >/dev/null
    fi
    if cargo build --quiet && ./target/debug/kythira_cache_probe >/dev/null; then
        ok=$((ok + 1))
        echo "KYTHIRA_BUILD build=${i} status=ok"
    else
        echo "KYTHIRA_BUILD build=${i} status=failed"
    fi
    if [[ "${mode}" == cached ]]; then
        # One line, so the test can pair a build with its stats.
        echo "KYTHIRA_STATS build=${i} $(sccache --show-stats --stats-format=json | tr -d '\n')"
    fi
done

if [[ "${mode}" == cached ]]; then
    sccache --stop-server >/dev/null 2>&1 || true
    if [[ -s "${SCCACHE_ERROR_LOG}" ]]; then
        echo "--- sccache server log ---"
        cat "${SCCACHE_ERROR_LOG}"
    fi
fi
echo "KYTHIRA_RESULT mode=${mode} builds=${builds} ok=${ok}"
[[ "${ok}" -eq "${builds}" ]]
