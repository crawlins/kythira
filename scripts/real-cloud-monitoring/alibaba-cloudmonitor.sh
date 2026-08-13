#!/usr/bin/env bash
# scripts/real-cloud-monitoring/alibaba-cloudmonitor.sh — real-service test
# for the Alibaba Cloud CloudMonitor monitoring example config (doc/TODO.md
# "Metrics Backends", Alibaba Cloud CloudMonitor entry, real-cloud tier).
#
# Runs a real OpenTelemetry Collector with the unmodified example config
# (docker/cloud-monitoring/alibaba-cloudmonitor-collector-config.yaml)
# against a real CloudMonitor 2.0 Prometheus instance (remote write), posts
# one synthetic OTLP metric, then confirms through the instance's own
# Prometheus-compatible query API that the series arrived.
#
# Environment:
#   ALIBABA_CLOUD_ACCESS_KEY_ID / ALIBABA_CLOUD_ACCESS_KEY_SECRET —
#     AccessKey pair (repo secrets), consumed by the example config's
#     basicauth extension and reused for the query below.
#   KYTHIRA_CMS_REMOTE_WRITE_URL — the instance's remote-write URL
#     (.../api/v3/write, repo variable ALIBABA_CMS_REMOTE_WRITE_URL).
#   KYTHIRA_CMS_PROM_QUERY_URL — the same instance's HTTP query API base
#     URL (repo variable ALIBABA_CMS_PROM_QUERY_URL).
#
# NOTE: no Alibaba Cloud account is provisioned for this project yet
# (doc/TODO.md Cloud Provider Support) — this script has never run against
# the live service and stays fail-closed until an operator provisions the
# secrets/variables above. See scripts/ci-cloud-credentials/alibaba/README.md.

set -euo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env ALIBABA_CLOUD_ACCESS_KEY_ID ALIBABA_CLOUD_ACCESS_KEY_SECRET \
    KYTHIRA_CMS_REMOTE_WRITE_URL KYTHIRA_CMS_PROM_QUERY_URL

RUN_ID="${GITHUB_RUN_ID:-local-$$}"

trap stop_collector EXIT

install_collector
start_collector docker/cloud-monitoring/alibaba-cloudmonitor-collector-config.yaml
post_otlp_probe "${RUN_ID}"

check_series_queryable() {
    curl -fsS -u "${ALIBABA_CLOUD_ACCESS_KEY_ID}:${ALIBABA_CLOUD_ACCESS_KEY_SECRET}" \
        "${KYTHIRA_CMS_PROM_QUERY_URL%/}/api/v1/query?query=${KYTHIRA_MONITORING_PROBE_METRIC}" \
        | grep -q "${KYTHIRA_MONITORING_PROBE_METRIC}"
}

poll_until 300 10 "probe series queryable in CloudMonitor Prometheus instance" check_series_queryable

echo "[monitoring] Alibaba CloudMonitor monitoring config verified against the real service."
