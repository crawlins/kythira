#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One leg of the disk-backed build cache's full-tree measurement
# (.kiro/specs/disk-backed-build-cache/ task 11, Requirement 13), run by
# .github/workflows/cache-cluster-measure.yml.
#
#   measure.sh cluster-up                start the three-node cluster, wait
#   measure.sh build <label> <backend>   clean configure + timed full build
#   measure.sh cluster-stats <label>     node RSS, store bytes and files
#
# <backend> is one of:
#   webdav      sccache 0.17.0's WebDAV backend on cache-1, read_write user
#   webdav-ro   the same with the read_only user and
#               SCCACHE_WEBDAV_RW_MODE=READ_ONLY (task 1 found the mode)
#   oci         the OCI bucket, as ci.yml uses it (the environment the
#               oci-build-cache action exported)
#
# Each build starts a fresh sccache server with only that backend's
# variables, so the S3 variables the oci-build-cache action exports cannot
# leak into a WebDAV run (sccache would pick S3 over WebDAV). It configures
# exactly as ci.yml's build-and-test "Configure (Release)" step does and
# times the build alone, which is what that job's "Build" step reports.
#
# Results land in $MEASURE_OUT (one file per label) and in the job summary.

set -euo pipefail

: "${MEASURE_COMPILER:?}"
: "${MEASURE_PREFIX_PATH:?}"
out="${MEASURE_OUT:-${RUNNER_TEMP:-/tmp}/measure}"
compose_file="services/redis/docker/cache-cluster/compose.yml"
cache_http="http://127.0.0.1:18091"
mkdir -p "$out"

summary() {
    echo "$*" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
}

metric() {  # metric <node 1..3> <name>
    curl -sf -u ops:ops-secret "http://127.0.0.1:1809$1/_metrics" \
        | awk -v n="kythira_cache_http_$2" '$1 == n { print $2 }'
}

metric_sum() {  # metric_sum <name>: over all three nodes. A PUT to a
    # follower is forwarded and counted in `puts` by the leader; a refused
    # one is counted in `forbidden` by the node that refused it.
    local total=0 n
    for n in 1 2 3; do
        total=$((total + $(metric "$n" "$1")))
    done
    echo "$total"
}

start_server() {  # start_server <backend>
    sccache --stop-server >/dev/null 2>&1 || true
    local -a env_cmd=(env -u SCCACHE_BUCKET -u SCCACHE_ENDPOINT -u SCCACHE_REGION
        -u SCCACHE_S3_KEY_PREFIX -u SCCACHE_S3_USE_SSL -u SCCACHE_S3_RW_MODE
        -u SCCACHE_WEBDAV_ENDPOINT -u SCCACHE_WEBDAV_USERNAME
        -u SCCACHE_WEBDAV_PASSWORD -u SCCACHE_WEBDAV_RW_MODE)
    case "$1" in
        webdav)
            env_cmd+=(SCCACHE_WEBDAV_ENDPOINT="${cache_http}/sccache"
                SCCACHE_WEBDAV_USERNAME=ci-main SCCACHE_WEBDAV_PASSWORD=ci-main-secret)
            ;;
        webdav-ro)
            env_cmd+=(SCCACHE_WEBDAV_ENDPOINT="${cache_http}/sccache"
                SCCACHE_WEBDAV_USERNAME=ci-pr SCCACHE_WEBDAV_PASSWORD=ci-pr-secret
                SCCACHE_WEBDAV_RW_MODE=READ_ONLY)
            ;;
        oci)
            # The action's variables, untouched.
            env_cmd=(env)
            ;;
        *)
            echo "unknown backend $1" >&2
            exit 64
            ;;
    esac
    local gate="$out/gate-$1"
    : > "$gate"
    "${env_cmd[@]}" GITHUB_OUTPUT="$gate" scripts/oci-build-cache/start-sccache.sh
    grep -qx 'launcher=sccache' "$gate"
}

case "${1:-}" in
cluster-up)
    docker compose -f "$compose_file" down -v --remove-orphans >/dev/null 2>&1 || true
    docker compose -f "$compose_file" up -d
    for _ in $(seq 1 120); do
        ok=0
        for n in 1 2 3; do
            curl -sf -o /dev/null -u ops:ops-secret "http://127.0.0.1:1809$n/_metrics" && ok=$((ok + 1))
        done
        # A one-byte PUT committing means both shards have a leader.
        if [[ $ok -eq 3 ]] && curl -sf -o /dev/null -u ci-main:ci-main-secret -T /dev/null \
            "${cache_http}/sccache/ready-probe"; then
            echo "cluster ready"
            exit 0
        fi
        sleep 1
    done
    docker compose -f "$compose_file" logs --tail 100
    exit 1
    ;;

build)
    label="$2"
    backend="$3"
    start_server "$backend"
    forbidden_before=$(metric_sum forbidden)
    puts_before=$(metric_sum puts)
    rm -rf build
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER="$MEASURE_COMPILER" \
        -DKYTHIRA_KCONFIG=configs/ci_full_defconfig \
        -DKYTHIRA_KCONFIG_STRICT=ON \
        -DKYTHIRA_COMPILER_LAUNCHER=sccache \
        -DKYTHIRA_LINKER=mold \
        -DKYTHIRA_TEST_TIMEOUT_SCALE=4 \
        -DCMAKE_PREFIX_PATH="$MEASURE_PREFIX_PATH" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$out/configure-$label.log"
    start=$(date +%s.%N)
    cmake --build build -j"$(nproc)" >"$out/build-$label.log" 2>&1 \
        || { tail -60 "$out/build-$label.log"; exit 1; }
    end=$(date +%s.%N)
    seconds=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.1f", b - a }')

    sccache --show-stats >"$out/stats-$label.txt"
    sccache --show-stats --stats-format=json >"$out/stats-$label.json"
    sccache --stop-server >/dev/null 2>&1 || true
    forbidden=$(( $(metric_sum forbidden) - forbidden_before ))
    puts=$(( $(metric_sum puts) - puts_before ))
    # cache_errors and cache_timeouts are per-language maps; sum them.
    jq --arg label "$label" --arg backend "$backend" --arg compiler "$MEASURE_COMPILER" \
        --argjson seconds "$seconds" --argjson forbidden "$forbidden" --argjson puts "$puts" '
        def total(f): (f.counts // {}) | add // 0;
        .stats as $s
        | (total($s.cache_hits)) as $hits
        | (total($s.cache_misses)) as $misses
        | {label: $label, backend: $backend, compiler: $compiler,
           build_seconds: $seconds,
           requests: $s.compile_requests,
           hits: $hits, misses: $misses,
           hit_rate: (if $hits + $misses > 0 then $hits / ($hits + $misses) else null end),
           not_cacheable: $s.requests_not_cacheable,
           read_errors: ($s.cache_read_errors + total($s.cache_errors)),
           write_errors: $s.cache_write_errors,
           timeouts: $s.cache_timeouts,
           writes: $s.cache_writes,
           avg_read_hit_ms: (if $s.cache_read_hit_duration and $hits > 0
                             then ($s.cache_read_hit_duration.secs * 1000
                                   + $s.cache_read_hit_duration.nanos / 1e6) / $hits
                             else null end),
           cluster_puts: $puts, cluster_forbidden: $forbidden}' \
        "$out/stats-$label.json" >"$out/result-$label.json"
    cat "$out/result-$label.json"

    summary "### $label ($MEASURE_COMPILER, $backend)"
    summary ""
    summary "$(jq -r '"Build \(.build_seconds) s (\(.build_seconds / 60 * 100 | floor / 100) min); hits \(.hits) / misses \(.misses) (hit rate \(if .hit_rate then (.hit_rate * 10000 | floor / 100 | tostring) + "%" else "n/a" end)); read errors \(.read_errors), write errors \(.write_errors), timeouts \(.timeouts); cluster PUTs \(.cluster_puts), 403s \(.cluster_forbidden)"' "$out/result-$label.json")"
    summary ""
    summary '```'
    summary "$(cat "$out/stats-$label.txt")"
    summary '```'
    rm -rf build
    ;;

cluster-stats)
    label="$2"
    {
        echo "{"
        for n in 1 2 3; do
            status=$(docker exec "cache-cluster-$n" cat /proc/1/status)
            hwm=$(awk '$1 == "VmHWM:" { print $2 }' <<<"$status")
            rss=$(awk '$1 == "VmRSS:" { print $2 }' <<<"$status")
            echo "  \"cache-$n\": {\"peak_rss_kib\": $hwm, \"rss_kib\": $rss,"
            echo "    \"store_bytes\": $(metric "$n" store_bytes_on_disk),"
            echo "    \"loose_files\": $(metric "$n" store_loose_files),"
            echo "    \"packed_chunks\": $(metric "$n" store_packed_chunks),"
            echo "    \"files_on_disk\": $(docker exec "cache-cluster-$n" sh -c 'find /data -type f | wc -l'),"
            echo "    \"hash_failures\": $(metric "$n" store_hash_failures)}$([[ $n -lt 3 ]] && echo ,)"
        done
        echo "}"
    } >"$out/cluster-$label.json"
    jq . "$out/cluster-$label.json"
    summary "### Cluster after $label"
    summary ""
    summary '```json'
    summary "$(jq . "$out/cluster-$label.json")"
    summary '```'
    ;;

*)
    echo "usage: measure.sh cluster-up | build <label> <webdav|webdav-ro|oci> | cluster-stats <label>" >&2
    exit 64
    ;;
esac
