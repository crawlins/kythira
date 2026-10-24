#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

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
#   2. installs the Management Agent on the runner itself (so the example
#      config's localhost:9464 url works verbatim) using the
#      operator-minted installer + install key. Not in a container: the
#      installer registers a systemd or init.d service and aborts when it
#      finds neither, which is what the first live run inside an
#      oraclelinux:8 container did (40125075314). The ubuntu-24.04 runner
#      has systemd and passwordless sudo and is thrown away after the job,
#      so the install needs no undoing; the script refuses to run outside
#      GitHub Actions rather than install a system service on a
#      workstation;
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
# NOTE: the installer URL and key were first provisioned on 2026-10-24
# (scripts/ci-cloud-credentials/provision-monitoring-tests.sh). The script
# fails closed without them, and the response-file keys below should be
# checked against the input.rsp.example inside the ZIP if a run rejects
# them.

set -euo pipefail
if [ "${GITHUB_ACTIONS:-}" != "true" ]; then
    echo "oci-monitoring.sh installs the OCI Management Agent as a system service" >&2
    echo "on this machine, so it only runs on a throwaway GitHub Actions runner." >&2
    exit 1
fi
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/real-cloud-monitoring/common.sh
source scripts/real-cloud-monitoring/common.sh

require_env KYTHIRA_OCI_SECURITY_TOKEN KYTHIRA_OCI_PRIVATE_KEY_PEM \
    KYTHIRA_OCI_REGION KYTHIRA_OCI_COMPARTMENT_ID \
    OCI_MGMT_AGENT_INSTALL_KEY OCI_MGMT_AGENT_INSTALLER_URL

RUN_ID="${GITHUB_RUN_ID:-local-$$}"
AGENT_HOME="/opt/oracle/mgmt_agent"
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
    sudo tail -50 "${AGENT_HOME}/agent_inst/log/mgmt_agent.log" 2>/dev/null || true
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

# ── 2+3. Agent on the runner with the substituted example config. ──
sed "s|^compartmentId=.*|compartmentId=${KYTHIRA_OCI_COMPARTMENT_ID}|" \
    docker/cloud-monitoring/oci-management-agent-prometheus-emitter.properties \
    > "${WORK_DIR}/kythira.properties"

printf '%s\n' \
    "ManagementAgentInstallKey = ${OCI_MGMT_AGENT_INSTALL_KEY}" \
    "AgentDisplayName = ${AGENT_DISPLAY_NAME}" \
    > "${WORK_DIR}/input.rsp"

curl -fsSL --retry 3 -o "${WORK_DIR}/omagent.zip" "${OCI_MGMT_AGENT_INSTALLER_URL}"

mkdir "${WORK_DIR}/omagent"
unzip -q "${WORK_DIR}/omagent.zip" -d "${WORK_DIR}/omagent"
# Find installer.sh rather than assume a layout: the comment here used to
# say the ZIP unpacks to a versioned oracle.mgmt_agent* directory, but the
# first live run (39979774036) found no such directory, so the installer
# sits at the top level or one directory down.
installer="$(find "${WORK_DIR}/omagent" -maxdepth 2 -name installer.sh -print -quit)"
if [ -z "${installer}" ]; then
    echo "::error::installer.sh not found in the agent ZIP; it holds:" >&2
    find "${WORK_DIR}/omagent" -maxdepth 2 | head -40 >&2
    exit 1
fi
# The installer creates the mgmt_agent user and then configures the agent
# as that user, which must read the response file: the second live run
# (40126837668) failed with "input.rsp file is either not present or not
# readable to mgmt_agent user". So the work directory becomes traversable
# (711, not listable) and only input.rsp readable; the session key and
# token stay 600 under the umask set in setup_cli. The runner has no other
# users. The installer brings its own JDK, so none is installed here.
chmod 711 "${WORK_DIR}"
chmod 644 "${WORK_DIR}/input.rsp"
(cd "$(dirname "${installer}")" && sudo bash installer.sh "${WORK_DIR}/input.rsp")
discovery="${AGENT_HOME}/agent_inst/discovery/PrometheusEmitter"
sudo mkdir -p "${discovery}"
sudo cp "${WORK_DIR}/kythira.properties" "${discovery}/"
# The agent runs as its own user: give the file the discovery tree's owner.
sudo chown -R --reference="${AGENT_HOME}/agent_inst/discovery" "${discovery}"

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
