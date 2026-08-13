#!/usr/bin/env bash
# scripts/real-cloud-monitoring/oci-monitoring.sh — real-service test for
# the OCI Monitoring example config (doc/TODO.md "Metrics Backends", OCI
# Monitoring entry, real-cloud tier).
#
# The OCI entry's routing mechanism is the vendor's own agent, not an OTel
# Collector (no OCI Monitoring exporter exists in collector-contrib): the
# OCI Management Agent's PrometheusEmitter scrapes a Prometheus endpoint
# and posts to OCI Monitoring's PostMetricData API. So this test:
#
#   1. serves a static Prometheus text exposition on localhost:9464 — the
#      same exposition format prometheus_scrape_server emits — containing
#      one known gauge;
#   2. installs the Management Agent in an oraclelinux:8 container
#      (--network host, so the example config's localhost:9464 url works
#      verbatim) using the operator-minted installer + install key;
#   3. drops the example .properties
#      (docker/cloud-monitoring/oci-management-agent-prometheus-emitter.
#      properties) into the agent's PrometheusEmitter discovery directory,
#      with only the deployment-specific compartmentId substituted — the
#      same substitution an operator performs;
#   4. polls OCI Monitoring's summarize-metrics-data API until the gauge
#      arrives under the example config's namespace;
#   5. deregisters the agent (agents otherwise accumulate in the tenancy).
#
# Environment (see scripts/ci-cloud-credentials/oci/README.md, Monitoring
# section, for provisioning):
#   KYTHIRA_OCI_SECURITY_TOKEN / KYTHIRA_OCI_PRIVATE_KEY_PEM /
#   KYTHIRA_OCI_REGION / KYTHIRA_OCI_COMPARTMENT_ID — the same UPST bundle
#     the other OCI real-cloud steps export.
#   OCI_MGMT_AGENT_INSTALL_KEY — a Management Agent install key (secret).
#   OCI_MGMT_AGENT_INSTALLER_URL — URL of the vendor's Linux (x86_64) ZIP
#     installer, e.g. a pre-authenticated request minted after downloading
#     it once from the console (Management Agents → Downloads and Keys).
#
# NOTE: this script has never run against a live tenancy (the installer
# URL/key are not provisioned yet) — it stays fail-closed until an operator
# supplies them, and the response-file keys below should be checked against
# the input.rsp.example shipped inside the downloaded installer ZIP on
# first use.

set -euo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env KYTHIRA_OCI_SECURITY_TOKEN KYTHIRA_OCI_PRIVATE_KEY_PEM \
    KYTHIRA_OCI_REGION KYTHIRA_OCI_COMPARTMENT_ID \
    OCI_MGMT_AGENT_INSTALL_KEY OCI_MGMT_AGENT_INSTALLER_URL

RUN_ID="${GITHUB_RUN_ID:-local-$$}"
AGENT_CONTAINER="kythira-oci-mgmt-agent"
AGENT_DISPLAY_NAME="kythira-ci-monitoring-${RUN_ID}"
WORK_DIR="$(mktemp -d)"
EXPOSITION_PID=""

# ── UPST-authenticated OCI CLI config (same shape as the real-cloud-tests
# oci job's audit step: tenancy read from the token's own tenant claim). ──
setup_cli() {
    pip3 install --user --quiet oci-cli
    export PATH="$HOME/.local/bin:$PATH"
    umask 077
    printf '%s' "${KYTHIRA_OCI_SECURITY_TOKEN}" > "${WORK_DIR}/upst.token"
    printf '%s\n' "${KYTHIRA_OCI_PRIVATE_KEY_PEM}" > "${WORK_DIR}/session.pem"
    local payload pad tenancy
    payload="$(printf '%s' "${KYTHIRA_OCI_SECURITY_TOKEN}" | cut -d. -f2 | tr '_-' '/+')"
    pad=$(( (4 - ${#payload} % 4) % 4 ))
    [ ${pad} -gt 0 ] && payload="${payload}$(printf '=%.0s' $(seq ${pad}))"
    tenancy="$(printf '%s' "${payload}" | base64 -d 2>/dev/null | jq -r '.tenant // empty')"
    if [ -z "${tenancy}" ]; then
        echo "::error::the UPST carries no tenant claim — cannot build a CLI config." >&2
        return 1
    fi
    mkdir -p ~/.oci
    printf '%s\n' \
        '[DEFAULT]' \
        "region=${KYTHIRA_OCI_REGION}" \
        "tenancy=${tenancy}" \
        "security_token_file=${WORK_DIR}/upst.token" \
        "key_file=${WORK_DIR}/session.pem" \
        > ~/.oci/config
}

cleanup() {
    [ -n "${EXPOSITION_PID}" ] && kill "${EXPOSITION_PID}" 2>/dev/null || true
    docker logs "${AGENT_CONTAINER}" 2>/dev/null | tail -50 || true
    docker rm -f "${AGENT_CONTAINER}" 2>/dev/null || true
    # Deregister the agent this run created — agents are per-install
    # tenancy objects, not per-container processes, and leak otherwise.
    local agent_id
    agent_id="$(oci --auth security_token management-agent agent list \
        --compartment-id "${KYTHIRA_OCI_COMPARTMENT_ID}" \
        --display-name "${AGENT_DISPLAY_NAME}" \
        --query 'data[0].id' --raw-output 2>/dev/null || true)"
    if [ -n "${agent_id}" ] && [ "${agent_id}" != "null" ]; then
        oci --auth security_token management-agent agent delete \
            --agent-id "${agent_id}" --force 2>/dev/null || \
            echo "::warning::could not deregister management agent ${agent_id} — remove it by hand (display name ${AGENT_DISPLAY_NAME})."
    fi
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

setup_cli

# ── 1. The scrape target: static exposition on :9464, matching the example
# config's url verbatim. ──
mkdir -p "${WORK_DIR}/www"
printf '%s\n' \
    "# TYPE ${KYTHIRA_MONITORING_PROBE_METRIC} gauge" \
    "${KYTHIRA_MONITORING_PROBE_METRIC}{run_id=\"${RUN_ID}\"} 1" \
    > "${WORK_DIR}/www/metrics"
(cd "${WORK_DIR}/www" && python3 -m http.server 9464 >/dev/null 2>&1) &
EXPOSITION_PID=$!

# ── 2+3. Agent container with the substituted example config. ──
sed "s|^compartmentId=.*|compartmentId=${KYTHIRA_OCI_COMPARTMENT_ID}|" \
    docker/cloud-monitoring/oci-management-agent-prometheus-emitter.properties \
    > "${WORK_DIR}/kythira.properties"

printf '%s\n' \
    "ManagementAgentInstallKey = ${OCI_MGMT_AGENT_INSTALL_KEY}" \
    "AgentDisplayName = ${AGENT_DISPLAY_NAME}" \
    > "${WORK_DIR}/input.rsp"

curl -fsSL --retry 3 -o "${WORK_DIR}/omagent.zip" "${OCI_MGMT_AGENT_INSTALLER_URL}"

docker run -d --name "${AGENT_CONTAINER}" --network host \
    -v "${WORK_DIR}:/bootstrap:ro" \
    oraclelinux:8 sleep infinity
docker exec "${AGENT_CONTAINER}" bash -c '
    set -euo pipefail
    dnf -y -q install java-11-openjdk unzip
    mkdir /tmp/omagent && cd /tmp/omagent
    unzip -q /bootstrap/omagent.zip
    # The ZIP unpacks to a versioned directory containing installer.sh.
    cd oracle.mgmt_agent*
    bash installer.sh /bootstrap/input.rsp
    mkdir -p /opt/oracle/mgmt_agent/agent_inst/discovery/PrometheusEmitter
    cp /bootstrap/kythira.properties \
        /opt/oracle/mgmt_agent/agent_inst/discovery/PrometheusEmitter/
'

# ── 4. The assertion, through OCI Monitoring's own query API. The emitter
# scrapes on scheduleMins=1 and PostMetricData latency is seconds, but
# first agent registration + plugin start is minutes — hence the deadline. ──
check_metric_posted() {
    local now start
    now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    start="$(date -u -d '-30 minutes' +%Y-%m-%dT%H:%M:%SZ)"
    oci --auth security_token monitoring metric-data summarize-metrics-data \
        --compartment-id "${KYTHIRA_OCI_COMPARTMENT_ID}" \
        --namespace kythira_chaos_node \
        --query-text "${KYTHIRA_MONITORING_PROBE_METRIC}[1m].max()" \
        --start-time "${start}" --end-time "${now}" \
        --output json | grep -q "${KYTHIRA_MONITORING_PROBE_METRIC}"
}

poll_until 900 30 "probe metric posted to OCI Monitoring (namespace kythira_chaos_node)" \
    check_metric_posted

echo "[monitoring] OCI Monitoring config verified against the real service."
