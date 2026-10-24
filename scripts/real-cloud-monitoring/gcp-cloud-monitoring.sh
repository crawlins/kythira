#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# scripts/real-cloud-monitoring/gcp-cloud-monitoring.sh — real-service test
# for the GCP Cloud Monitoring monitoring example config (doc/TODO.md
# "Metrics Backends", GCP Cloud Monitoring entry, real-cloud tier).
#
# Runs a real OpenTelemetry Collector with the unmodified example config
# (docker/cloud-monitoring/gcp-cloud-monitoring-collector-config.yaml)
# against real Cloud Monitoring, posts one synthetic OTLP metric, then
# confirms through the timeSeries.list API that it arrived as
# workload.googleapis.com/kythira_ci_monitoring_probe.
#
# Environment:
#   GCP_PROJECT_ID — the project to write/read (repo variable
#     GCP_REAL_CLOUD_TESTS_PROJECT). Consumed by the example config too.
#   GOOGLE_APPLICATION_CREDENTIALS — workload-identity credential file
#     (google-github-actions/auth@v2 exports it); the collector's ADC and
#     gcloud's access token both come from it. The CI service account needs
#     roles/monitoring.metricWriter + roles/logging.logWriter (ingest) and
#     roles/monitoring.viewer (the assertion).

set -euo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env GCP_PROJECT_ID GOOGLE_APPLICATION_CREDENTIALS

RUN_ID="${GITHUB_RUN_ID:-local-$$}"

trap stop_collector EXIT

install_collector
start_collector docker/cloud-monitoring/gcp-cloud-monitoring-collector-config.yaml
post_otlp_probe "${RUN_ID}"

check_timeseries_listed() {
    local token start end
    token="$(gcloud auth print-access-token)"
    start="$(date -u -d '-30 minutes' +%Y-%m-%dT%H:%M:%SZ)"
    end="$(date -u -d '+5 minutes' +%Y-%m-%dT%H:%M:%SZ)"
    if curl -fsS -H "Authorization: Bearer ${token}" \
        "https://monitoring.googleapis.com/v3/projects/${GCP_PROJECT_ID}/timeSeries?filter=metric.type%3D%22workload.googleapis.com%2F${KYTHIRA_MONITORING_PROBE_METRIC}%22&interval.startTime=${start}&interval.endTime=${end}" \
        | grep -q "${KYTHIRA_MONITORING_PROBE_METRIC}"; then
        return 0
    fi
    # Not there yet: post a fresh point. The googlecloud exporter drops a
    # batch that timeSeries.create rejects instead of retrying it, and the
    # first write of a new metric type can fail with a transient INTERNAL
    # while Cloud Monitoring creates its descriptor; the first live run
    # (40125075314) lost its only probe that way and then polled for 300 s
    # for a point that was never coming. A real chaos node exports every
    # interval, so re-posting is what the config sees in use. The 10 s poll
    # interval keeps points above the 5 s minimum a gauge series accepts.
    post_otlp_probe "${RUN_ID}" >/dev/null
    return 1
}

poll_until 300 10 "probe time series listed in Cloud Monitoring" check_timeseries_listed

echo "[monitoring] GCP Cloud Monitoring config verified against the real service."
