#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# scripts/real-cloud-monitoring/azure-monitor.sh — real-service test for
# the Azure Monitor monitoring example config (doc/TODO.md "Metrics
# Backends", Azure Monitor entry, real-cloud tier).
#
# Runs a real OpenTelemetry Collector with the unmodified example config
# (docker/cloud-monitoring/azure-monitor-collector-config.yaml) against a
# real Application Insights resource, posts one synthetic OTLP metric,
# then confirms through the resource's own query API (KQL over
# customMetrics) that the metric arrived.
#
# Environment:
#   APPLICATIONINSIGHTS_CONNECTION_STRING — the resource's connection
#     string (repo secret AZURE_MONITORING_CONNECTION_STRING; it authorizes
#     ingestion). Consumed by the example config via ${env:...}.
#   AZURE_MONITORING_APP_ID — the resource's Application ID (repo
#     variable), used for the query-side assertion.
#   An authenticated az CLI session (the workflow's azure/login step) whose
#   principal can read the resource (Reader / Monitoring Reader).

set -euo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env APPLICATIONINSIGHTS_CONNECTION_STRING AZURE_MONITORING_APP_ID

RUN_ID="${GITHUB_RUN_ID:-local-$$}"

trap stop_collector EXIT

# The query below needs the application-insights az extension; allow az to
# install it without an interactive prompt.
az config set extension.use_dynamic_install=yes_without_prompt >/dev/null

install_collector
start_collector docker/cloud-monitoring/azure-monitor-collector-config.yaml
post_otlp_probe "${RUN_ID}"

check_metric_ingested() {
    az monitor app-insights query \
        --app "${AZURE_MONITORING_APP_ID}" \
        --analytics-query "customMetrics | where name == '${KYTHIRA_MONITORING_PROBE_METRIC}' | where timestamp > ago(1h) | take 1" \
        --output json | grep -q "${KYTHIRA_MONITORING_PROBE_METRIC}"
}

# Application Insights ingestion latency is documented in minutes, not
# seconds — the generous deadline is the vendor's, not ours.
poll_until 600 20 "probe metric queryable in Application Insights" check_metric_ingested

echo "[monitoring] Azure Monitor monitoring config verified against the real service."
