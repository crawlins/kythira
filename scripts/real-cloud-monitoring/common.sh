#!/usr/bin/env bash
# scripts/real-cloud-monitoring/common.sh — shared machinery for the
# cloud-vendor monitoring real-service tests (doc/TODO.md "Metrics
# Backends", cloud-vendor entries, real-cloud tier; operator docs in
# doc/cloud_vendor_monitoring.md).
#
# Sourced (not executed) by the per-vendor scripts in this directory. Each
# of those stands up the routing mechanism its example config describes — a
# real OpenTelemetry Collector running the UNMODIFIED file from
# docker/cloud-monitoring/ — sends one synthetic OTLP metric (the same
# OTLP/HTTP JSON wire shape otlp_metrics emits), and polls the vendor's own
# query API until the metric arrives. Deliberately NOT a chaos_node run:
# the Docker tier (tests/docker_chaos/) already proves Kythira's own
# telemetry traverses these configs; this tier's question is whether the
# example config really reaches the real vendor service, which a synthetic
# sample answers without dragging a full C++ build into every monitoring
# job.
#
# The Collector runs as a host binary (GitHub release tarball, pinned to
# the same version the Docker tiers pin) rather than a container: the GCP
# leg authenticates via a workload-identity credential *file* whose
# absolute paths would otherwise all need bind-mount mirroring, and a host
# process sidesteps that class of problem for every vendor at once.

set -euo pipefail

# Same version as docker/otlp-collector-compose.yml and the Docker-tier
# validation test — one pinned Collector everywhere.
OTELCOL_VERSION="0.116.1"

KYTHIRA_MONITORING_PROBE_METRIC="kythira_ci_monitoring_probe"
KYTHIRA_MONITORING_PROBE_LOG_BODY="kythira ci monitoring probe log"

_otelcol_dir=""
_otelcol_pid=""
_otelcol_log=""

# require_env VAR [VAR...] — fail closed, naming every unset variable, so a
# misconfigured job says which knob is missing instead of failing somewhere
# downstream with a vendor error.
require_env() {
    local fail=0
    for var in "$@"; do
        if [ -z "${!var:-}" ]; then
            echo "::error::${var} is unset but required by $(basename "$0"). See doc/cloud_vendor_monitoring.md and scripts/ci-cloud-credentials/." >&2
            fail=1
        fi
    done
    return $fail
}

install_collector() {
    local arch
    case "$(uname -m)" in
        x86_64) arch=amd64 ;;
        aarch64) arch=arm64 ;;
        *) echo "::error::unsupported architecture $(uname -m)" >&2; return 1 ;;
    esac
    _otelcol_dir="$(mktemp -d)"
    local tarball="otelcol-contrib_${OTELCOL_VERSION}_linux_${arch}.tar.gz"
    echo "[monitoring] downloading otelcol-contrib ${OTELCOL_VERSION} (${arch})"
    curl -fsSL --retry 3 -o "${_otelcol_dir}/${tarball}" \
        "https://github.com/open-telemetry/opentelemetry-collector-releases/releases/download/v${OTELCOL_VERSION}/${tarball}"
    tar -xzf "${_otelcol_dir}/${tarball}" -C "${_otelcol_dir}" otelcol-contrib
}

# start_collector CONFIG_FILE — starts the Collector in the background with
# the caller's already-exported environment (that is how the example
# configs take credentials/endpoints) and waits for the OTLP/HTTP receiver
# to accept connections.
start_collector() {
    local config="$1"
    _otelcol_log="${_otelcol_dir}/otelcol.log"
    "${_otelcol_dir}/otelcol-contrib" --config="${config}" >"${_otelcol_log}" 2>&1 &
    _otelcol_pid=$!
    for _ in $(seq 1 30); do
        # No -f: the OTLP receiver 404s a bare GET /, which still proves
        # the listener is up — only a connection failure should keep polling.
        if curl -sS -o /dev/null "http://127.0.0.1:4318" 2>/dev/null; then
            echo "[monitoring] collector up (pid ${_otelcol_pid})"
            return 0
        fi
        if ! kill -0 "${_otelcol_pid}" 2>/dev/null; then
            echo "::error::collector exited during startup — log follows" >&2
            cat "${_otelcol_log}" >&2
            return 1
        fi
        sleep 1
    done
    echo "::error::collector never opened :4318 — log follows" >&2
    cat "${_otelcol_log}" >&2
    return 1
}

# stop_collector — SIGTERM (flushes exporter queues on shutdown), then dump
# the log so a vendor-side rejection is visible in the CI log even when the
# assertion already failed for lack of data.
stop_collector() {
    if [ -n "${_otelcol_pid}" ] && kill -0 "${_otelcol_pid}" 2>/dev/null; then
        kill "${_otelcol_pid}" 2>/dev/null || true
        wait "${_otelcol_pid}" 2>/dev/null || true
    fi
    if [ -n "${_otelcol_log}" ] && [ -f "${_otelcol_log}" ]; then
        echo "[monitoring] collector log:"
        cat "${_otelcol_log}"
    fi
}

# post_otlp_probe RUN_ID — one gauge datapoint plus one log record, POSTed
# as OTLP/HTTP JSON to the local Collector. A gauge is the one shape every
# vendor exporter in the example configs maps without temporality
# conversion caveats.
post_otlp_probe() {
    local run_id="$1"
    local now_ns
    now_ns="$(date +%s%N)"
    curl -fsS -X POST -H 'Content-Type: application/json' \
        "http://127.0.0.1:4318/v1/metrics" -d @- <<EOF
{"resourceMetrics":[{"resource":{"attributes":[
  {"key":"service.name","value":{"stringValue":"kythira-ci-monitoring-probe"}},
  {"key":"service.instance.id","value":{"stringValue":"${run_id}"}}]},
 "scopeMetrics":[{"scope":{"name":"kythira-ci"},"metrics":[
  {"name":"${KYTHIRA_MONITORING_PROBE_METRIC}",
   "gauge":{"dataPoints":[{"asDouble":1,
     "timeUnixNano":"${now_ns}",
     "attributes":[{"key":"run_id","value":{"stringValue":"${run_id}"}}]}]}}]}]}]}
EOF
    echo "[monitoring] posted probe metric (run_id=${run_id})"
    curl -fsS -X POST -H 'Content-Type: application/json' \
        "http://127.0.0.1:4318/v1/logs" -d @- <<EOF
{"resourceLogs":[{"resource":{"attributes":[
  {"key":"service.name","value":{"stringValue":"kythira-ci-monitoring-probe"}},
  {"key":"service.instance.id","value":{"stringValue":"${run_id}"}}]},
 "scopeLogs":[{"scope":{"name":"kythira-ci"},"logRecords":[
  {"timeUnixNano":"${now_ns}","severityText":"INFO",
   "body":{"stringValue":"${KYTHIRA_MONITORING_PROBE_LOG_BODY} ${run_id}"}}]}]}]}
EOF
    echo "[monitoring] posted probe log (run_id=${run_id})"
}

# poll_until TIMEOUT_S INTERVAL_S DESCRIPTION CMD... — runs CMD until it
# succeeds or the deadline passes. CMD's stdout is suppressed while
# polling; the last attempt's output is shown on failure.
poll_until() {
    local timeout_s="$1" interval_s="$2" description="$3"
    shift 3
    local deadline=$((SECONDS + timeout_s))
    local out=""
    while [ ${SECONDS} -lt ${deadline} ]; do
        if out="$("$@" 2>&1)"; then
            echo "[monitoring] ${description}: confirmed after $((SECONDS))s"
            return 0
        fi
        sleep "${interval_s}"
    done
    echo "::error::${description}: not confirmed within ${timeout_s}s. Last attempt output:" >&2
    echo "${out}" >&2
    return 1
}
