#!/usr/bin/env bash
# scripts/real-cloud-monitoring/aws-cloudwatch.sh — real-service test for
# the AWS CloudWatch monitoring example config (doc/TODO.md "Metrics
# Backends", AWS CloudWatch entry, real-cloud tier).
#
# Runs a real OpenTelemetry Collector with the unmodified example config
# (docker/cloud-monitoring/cloudwatch-collector-config.yaml) against real
# CloudWatch, posts one synthetic OTLP metric + log, then confirms through
# CloudWatch's own APIs that (a) the metric was extracted from the EMF
# document into the Kythira/ChaosNode namespace and (b) the log record
# landed in the logs log group. Cleans up both log groups afterwards.
#
# Environment: ambient AWS credentials + AWS_REGION (the workflow's OIDC
# credentials step provides both). The CI role needs the
# cloudwatch-monitoring policy bundle
# (scripts/ci-cloud-credentials/aws/policies/cloudwatch-monitoring.json).

set -euo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env AWS_REGION

RUN_ID="${GITHUB_RUN_ID:-local-$$}"
METRICS_GROUP="/kythira/chaos-node/metrics"
LOGS_GROUP="/kythira/chaos-node/logs"

cleanup() {
    stop_collector
    # Best-effort hygiene: the groups are recreated by the exporters on the
    # next run, and a failed delete must not overwrite the test's verdict.
    aws logs delete-log-group --log-group-name "${METRICS_GROUP}" 2>/dev/null || true
    aws logs delete-log-group --log-group-name "${LOGS_GROUP}" 2>/dev/null || true
}
trap cleanup EXIT

install_collector
# Empty endpoint = the exporters' default (real AWS); exported explicitly so
# the ${env:...} reference resolves to the documented "unset for real AWS"
# state regardless of what the runner environment carries.
export KYTHIRA_CLOUDWATCH_ENDPOINT=""
start_collector docker/cloud-monitoring/cloudwatch-collector-config.yaml
post_otlp_probe "${RUN_ID}"

check_metric_extracted() {
    aws cloudwatch list-metrics \
        --namespace Kythira/ChaosNode \
        --metric-name "${KYTHIRA_MONITORING_PROBE_METRIC}" \
        --output text | grep -q "${KYTHIRA_MONITORING_PROBE_METRIC}"
}

check_log_arrived() {
    aws logs filter-log-events \
        --log-group-name "${LOGS_GROUP}" \
        --limit 50 --output text | grep -q "${KYTHIRA_MONITORING_PROBE_LOG_BODY}"
}

# EMF extraction is server-side and usually lands within a minute; 300s
# covers a slow region without letting a broken pipeline hold the runner.
poll_until 300 10 "probe metric extracted into Kythira/ChaosNode" check_metric_extracted
poll_until 120 10 "probe log record present in ${LOGS_GROUP}" check_log_arrived

echo "[monitoring] AWS CloudWatch monitoring config verified against the real service."
