#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Provisions what the four never-run monitoring-config jobs in
# real-cloud-tests.yml need (azure-monitoring, gcp-monitoring,
# oci-monitoring, alibaba-monitoring), writes the repository settings they
# read, and turns on each job's REAL_CLOUD_TESTS_<PROVIDER>_MONITORING_ENABLED
# toggle once its provider is ready. aws-monitoring is not touched.
#
#   azure    Log Analytics workspace + workspace-based Application Insights
#            kythira-ci-monitoring in kythira-realtest-rg, Monitoring Reader
#            on it for the CI identity (AZURE_CI_CLIENT_ID).
#            Writes variable AZURE_MONITORING_APP_ID and environment secret
#            AZURE_MONITORING_CONNECTION_STRING.
#   gcp      Enables the Monitoring and Logging APIs and grants the CI service
#            account (GCP_CI_SERVICE_ACCOUNT) monitoring.metricWriter,
#            logging.logWriter and monitoring.viewer on
#            GCP_REAL_CLOUD_TESTS_PROJECT. Writes no new settings.
#   oci      Management Agent install key kythira-ci-monitoring, the Linux
#            x86_64 agent ZIP copied into the kythira-ci-artifacts bucket
#            behind a one-year read PAR, dynamic group
#            kythira-ci-mgmt-agent-dg, and a separate policy
#            kythira-ci-monitoring (kept out of kythira-ci-policy, which
#            provision-ci-identity.sh replaces wholesale). Writes variable
#            OCI_MGMT_AGENT_INSTALLER_URL and environment secret
#            OCI_MGMT_AGENT_INSTALL_KEY.
#   alibaba  Provisions nothing (the CloudMonitor instance and RAM user exist
#            since August 20, 2026; see alibaba/README.md). Checks that its
#            two secrets and two variables are set, and enables it only then.
#
# Each provider runs on its own: one that fails or lacks its CLI is reported
# and skipped, and the others still run. Re-running is safe: every resource
# is looked up before it is created, and settings are rewritten with the
# current values. Toggles are repository variables, because the jobs' gate is
# a job-level `if:` that is evaluated before the environment binds.
#
# Cost: one datapoint per run per provider, billed by ingested volume; none
# of the resources has a standing fee.
#
# Needs `gh` (repo admin) plus, per provider, `az` (Owner or User Access
# Administrator on the resource group), `gcloud` (project IAM admin), and
# `oci` (tenancy admin profile).
#
# Usage:
#   scripts/ci-cloud-credentials/provision-monitoring-tests.sh
#       [--only azure,gcp,oci,alibaba] [--no-enable] [--dry-run]
#       [--repo OWNER/REPO] [--oci-profile NAME]
#       [--azure-resource-group RG] [--azure-location LOC]
#       [--oci-bucket NAME]
#
#   --only        providers to run (default: all four)
#   --no-enable   provision and write settings, but leave the toggles alone
#   --dry-run     print every write instead of doing it; reads still run
set -euo pipefail

REPO="crawlins/kythira"
ENVIRONMENT="real-cloud-tests"
ONLY="azure,gcp,oci,alibaba"
ENABLE=1
DRY_RUN=0
OCI_PROFILE="${OCI_CLI_PROFILE:-DEFAULT}"
AZ_RG="kythira-realtest-rg"
AZ_LOCATION="eastus"
OCI_BUCKET="kythira-ci-artifacts"
NAME="kythira-ci-monitoring"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) ONLY="$2"; shift 2 ;;
        --no-enable) ENABLE=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --repo) REPO="$2"; shift 2 ;;
        --oci-profile) OCI_PROFILE="$2"; shift 2 ;;
        --azure-resource-group) AZ_RG="$2"; shift 2 ;;
        --azure-location) AZ_LOCATION="$2"; shift 2 ;;
        --oci-bucket) OCI_BUCKET="$2"; shift 2 ;;
        # The header after the licence lines, matched on content rather
        # than a line range so editing the header never shifts it.
        -h|--help) awk '/^# Provisions what the four/{p=1} p&&!/^#/{exit} p{sub(/^# ?/,""); print}' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

for tool in gh jq; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} is not installed." >&2; exit 1; }
done
gh api "repos/${REPO}" --jq .full_name >/dev/null || { echo "ERROR: gh cannot read ${REPO}." >&2; exit 1; }

# Writes go through run(), so --dry-run prints them instead.
run() {
    if (( DRY_RUN )); then
        printf '  [dry-run]'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

# Repository variable value, or empty when it is unset (HTTP 404). `gh
# variable get` only exists from gh 2.47 on, so read the REST endpoint
# instead. Any other error is retried and then fails loudly: swallowing it
# once handed a later command an empty OCI compartment id.
repo_var() {
    local i err rc=0
    err=$(mktemp)
    for i in 1 2 3 4; do
        if gh api "repos/${REPO}/actions/variables/$1" --jq .value 2>"${err}"; then
            rm -f "${err}"; return 0
        fi
        if grep -q 'HTTP 404' "${err}"; then rm -f "${err}"; return 0; fi
        (( i < 4 )) && sleep $(( i * 2 ))
    done
    echo "ERROR: could not read repository variable $1: $(tr '\n' ' ' <"${err}")" >&2
    rm -f "${err}"
    rc=1
    return "${rc}"
}

# True when the secret exists at repository or environment level.
has_secret() {
    gh api "repos/${REPO}/actions/secrets/$1" >/dev/null 2>&1 \
        || gh api "repos/${REPO}/environments/${ENVIRONMENT}/secrets/$1" >/dev/null 2>&1
}

set_var() { run gh variable set "$1" -R "${REPO}" --body "$2"; }
# Secrets go to the environment the jobs bind; read from stdin so the value
# never appears in a process listing.
set_env_secret() { run gh secret set "$1" -R "${REPO}" --env "${ENVIRONMENT}"; }

need_vars() {
    local missing=() v
    for v in "$@"; do [[ -n "$(repo_var "$v")" ]] || missing+=("$v"); done
    if (( ${#missing[@]} )); then
        echo "ERROR: repository variable(s) unset: ${missing[*]}" >&2
        return 1
    fi
}

enable() {
    if (( ENABLE )); then
        set_var "REAL_CLOUD_TESTS_$1_MONITORING_ENABLED" true
        echo "REAL_CLOUD_TESTS_$1_MONITORING_ENABLED=true"
    else
        echo "--no-enable: leaving REAL_CLOUD_TESTS_$1_MONITORING_ENABLED as it is"
    fi
}

provision_azure() {
    command -v az >/dev/null || { echo "ERROR: az is not installed." >&2; return 1; }
    need_vars AZURE_CI_CLIENT_ID AZURE_CI_TENANT_ID AZURE_CI_SUBSCRIPTION_ID
    # An Application Insights resource set up by hand earlier is left as it
    # is: the job only needs its two settings, not these names.
    if [[ -n "$(repo_var AZURE_MONITORING_APP_ID)" ]] && has_secret AZURE_MONITORING_CONNECTION_STRING; then
        echo "AZURE_MONITORING_APP_ID and AZURE_MONITORING_CONNECTION_STRING already set: reusing them"
        enable AZURE
        return 0
    fi
    local sub client law_id ai_json ai_id app_id conn
    sub=$(repo_var AZURE_CI_SUBSCRIPTION_ID)
    client=$(repo_var AZURE_CI_CLIENT_ID)
    az config set extension.use_dynamic_install=yes_without_prompt >/dev/null 2>&1 || true

    if ! law_id=$(az monitor log-analytics workspace show --subscription "${sub}" \
            -g "${AZ_RG}" -n "${NAME}" --query id -o tsv 2>/dev/null); then
        echo "creating Log Analytics workspace ${NAME}"
        run az monitor log-analytics workspace create --subscription "${sub}" \
            -g "${AZ_RG}" -n "${NAME}" -l "${AZ_LOCATION}" -o none
        law_id=$(az monitor log-analytics workspace show --subscription "${sub}" \
            -g "${AZ_RG}" -n "${NAME}" --query id -o tsv 2>/dev/null || echo "<new workspace id>")
    else
        echo "Log Analytics workspace ${NAME}: exists"
    fi

    if ! ai_json=$(az monitor app-insights component show --subscription "${sub}" \
            -g "${AZ_RG}" --app "${NAME}" -o json 2>/dev/null); then
        echo "creating Application Insights ${NAME}"
        run az monitor app-insights component create --subscription "${sub}" \
            -g "${AZ_RG}" --app "${NAME}" -l "${AZ_LOCATION}" \
            --workspace "${law_id}" --kind other --application-type other -o none
        if (( DRY_RUN )); then
            echo "  [dry-run] would write AZURE_MONITORING_APP_ID and AZURE_MONITORING_CONNECTION_STRING"
            return 0
        fi
        ai_json=$(az monitor app-insights component show --subscription "${sub}" \
            -g "${AZ_RG}" --app "${NAME}" -o json)
    else
        echo "Application Insights ${NAME}: exists"
    fi
    ai_id=$(jq -r .id <<<"${ai_json}")
    app_id=$(jq -r .appId <<<"${ai_json}")
    conn=$(jq -r .connectionString <<<"${ai_json}")

    if [[ -z "$(az role assignment list --subscription "${sub}" --assignee "${client}" \
            --role "Monitoring Reader" --scope "${ai_id}" --query '[0].id' -o tsv 2>/dev/null)" ]]; then
        echo "granting Monitoring Reader on ${NAME} to ${client}"
        run az role assignment create --subscription "${sub}" --assignee "${client}" \
            --role "Monitoring Reader" --scope "${ai_id}" -o none
    else
        echo "Monitoring Reader on ${NAME}: already granted"
    fi

    set_var AZURE_MONITORING_APP_ID "${app_id}"
    printf '%s' "${conn}" | set_env_secret AZURE_MONITORING_CONNECTION_STRING
    echo "AZURE_MONITORING_APP_ID=${app_id}; AZURE_MONITORING_CONNECTION_STRING written"
    enable AZURE
}

provision_gcp() {
    command -v gcloud >/dev/null || { echo "ERROR: gcloud is not installed." >&2; return 1; }
    need_vars GCP_CI_SERVICE_ACCOUNT GCP_REAL_CLOUD_TESTS_PROJECT GCP_CI_WORKLOAD_IDENTITY_PROVIDER
    local project sa role
    project=$(repo_var GCP_REAL_CLOUD_TESTS_PROJECT)
    sa=$(repo_var GCP_CI_SERVICE_ACCOUNT)
    run gcloud services enable monitoring.googleapis.com logging.googleapis.com --project "${project}"
    for role in roles/monitoring.metricWriter roles/logging.logWriter roles/monitoring.viewer; do
        echo "granting ${role} to ${sa}"
        run gcloud projects add-iam-policy-binding "${project}" \
            --member "serviceAccount:${sa}" --role "${role}" --condition=None --quiet >/dev/null
    done
    enable GCP
}

provision_oci() {
    command -v oci >/dev/null || { echo "ERROR: oci is not installed." >&2; return 1; }
    need_vars OCI_CI_COMPARTMENT_ID OCI_CI_REGION
    for s in OCI_DOMAIN_URL OCI_WIF_CLIENT_ID OCI_WIF_CLIENT_SECRET; do
        has_secret "$s" || { echo "ERROR: secret ${s} is unset; the job's WIF sign-in needs it." >&2; return 1; }
    done
    export OCI_CLI_PROFILE="${OCI_PROFILE}"
    local comp region tenancy comp_name ns key_id key image obj_url src_ns src_bucket src_obj
    local zip par_name par_url new_id dg_name policy_id statements work
    comp=$(repo_var OCI_CI_COMPARTMENT_ID)
    region=$(repo_var OCI_CI_REGION)
    tenancy=$(oci iam compartment list --query 'data[0]."compartment-id"' --raw-output)
    comp_name=$(oci iam compartment get --compartment-id "${comp}" --query data.name --raw-output)
    ns=$(oci os ns get --query data --raw-output)
    echo "tenancy ${tenancy}; compartment ${comp_name}; region ${region}"

    # Install key. Its value is only readable at creation and via get, so
    # look it up by name and read it back either way.
    key_id=$(oci management-agent install-key list --compartment-id "${comp}" \
        --display-name "${NAME}" --lifecycle-state ACTIVE --all \
        --query 'data[0].id' --raw-output 2>/dev/null || true)
    if [[ -z "${key_id}" || "${key_id}" == "null" ]]; then
        echo "creating Management Agent install key ${NAME}"
        if (( DRY_RUN )); then
            run oci management-agent install-key create --compartment-id "${comp}" \
                --display-name "${NAME}" --is-unlimited true
            key_id=""
        else
            key_id=$(oci management-agent install-key create --compartment-id "${comp}" \
                --display-name "${NAME}" --is-unlimited true --query data.id --raw-output)
        fi
    else
        echo "install key ${NAME}: exists"
    fi
    if [[ -n "${key_id}" ]]; then
        key=$(oci management-agent install-key get --install-key-id "${key_id}" \
            --query data.key --raw-output)
        printf '%s' "${key}" | set_env_secret OCI_MGMT_AGENT_INSTALL_KEY
        echo "OCI_MGMT_AGENT_INSTALL_KEY written"
    fi

    # Agent ZIP: the image list points at Oracle's own bucket; copy it into
    # ours so the job's plain curl gets it through a PAR.
    image=$(oci management-agent agent-image list --compartment-id "${tenancy}" --all --output json \
        | jq -c '[.data[] | select(."platform-type"=="LINUX" and ."package-type"=="ZIP"
                   and ."package-architecture-type"=="X86_64")][0]')
    [[ -n "${image}" && "${image}" != "null" ]] || { echo "ERROR: no Linux x86_64 ZIP agent image listed." >&2; return 1; }
    obj_url=$(jq -r '."object-url"' <<<"${image}")
    zip="mgmt-agent/oracle.mgmt_agent-$(jq -r .version <<<"${image}")-linux-x86_64.zip"
    echo "agent image $(jq -r .version <<<"${image}") at ${obj_url}"
    if ! oci os object head -ns "${ns}" -bn "${OCI_BUCKET}" --name "${zip}" >/dev/null 2>&1; then
        work=$(mktemp -d)
        # .../n/<namespace>/b/<bucket>/o/<object>
        src_ns=$(sed -E 's#.*/n/([^/]+)/b/.*#\1#' <<<"${obj_url}")
        src_bucket=$(sed -E 's#.*/b/([^/]+)/o/.*#\1#' <<<"${obj_url}")
        src_obj=$(sed -E 's#.*/o/##' <<<"${obj_url}")
        if ! oci os object get -ns "${src_ns}" -bn "${src_bucket}" --name "${src_obj}" \
                --file "${work}/agent.zip" >/dev/null 2>&1; then
            curl -fsSL --retry 3 -o "${work}/agent.zip" "${obj_url}"
        fi
        unzip -tq "${work}/agent.zip" >/dev/null || { echo "ERROR: downloaded agent is not a ZIP." >&2; rm -rf "${work}"; return 1; }
        echo "uploading ${zip} to ${OCI_BUCKET}"
        run oci os object put -ns "${ns}" -bn "${OCI_BUCKET}" --name "${zip}" \
            --file "${work}/agent.zip" --force >/dev/null
        rm -rf "${work}"
    else
        echo "${OCI_BUCKET}/${zip}: exists"
    fi
    par_name="${NAME}-agent-zip"
    if (( DRY_RUN )); then
        run oci os preauth-request create -ns "${ns}" -bn "${OCI_BUCKET}" --name "${par_name}" \
            --access-type ObjectRead --object-name "${zip}" --time-expires "<now + 1 year>"
    else
        # A PAR's URL is only shown at creation, so each run mints a fresh
        # one, points the variable at it, then deletes the older ones.
        par_url=$(oci os preauth-request create -ns "${ns}" -bn "${OCI_BUCKET}" --name "${par_name}" \
            --access-type ObjectRead --object-name "${zip}" \
            --time-expires "$(date -u -d '+1 year' +%Y-%m-%dT%H:%M:%SZ)" \
            --query 'data."access-uri"' --raw-output)
        par_url="https://objectstorage.${region}.oraclecloud.com${par_url}"
        set_var OCI_MGMT_AGENT_INSTALLER_URL "${par_url}"
        echo "OCI_MGMT_AGENT_INSTALLER_URL written (PAR expires in a year)"
        new_id=$(oci os preauth-request list -ns "${ns}" -bn "${OCI_BUCKET}" --all --output json \
            | jq -r --arg n "${par_name}" '[.data[] | select(.name==$n)] | sort_by(."time-created") | last | .id')
        oci os preauth-request list -ns "${ns}" -bn "${OCI_BUCKET}" --all --output json \
            | jq -r --arg n "${par_name}" --arg keep "${new_id}" '.data[] | select(.name==$n and .id!=$keep) | .id' \
            | while read -r old; do
                oci os preauth-request delete -ns "${ns}" -bn "${OCI_BUCKET}" --par-id "${old}" --force >/dev/null
              done
    fi

    dg_name="kythira-ci-mgmt-agent-dg"
    if [[ -z "$(oci iam dynamic-group list --name "${dg_name}" --query 'data[0].id' --raw-output 2>/dev/null)" ]]; then
        echo "creating dynamic group ${dg_name}"
        run oci iam dynamic-group create --name "${dg_name}" \
            --description "Kythira CI Management Agents (oci-monitoring job)" \
            --matching-rule "ALL {resource.type = 'managementagent', resource.compartment.id = '${comp}'}" \
            --wait-for-state ACTIVE >/dev/null
    else
        echo "dynamic group ${dg_name}: exists"
    fi

    statements=$(jq -cn --arg c "${comp_name}" --arg dg "${dg_name}" '[
        "Allow dynamic-group \($dg) to use metrics in compartment \($c) where target.metrics.namespace = '\''kythira_chaos_node'\''",
        "Allow group kythira-ci to read metrics in compartment \($c)",
        "Allow group kythira-ci to manage management-agents in compartment \($c)"
    ]')
    policy_id=$(oci iam policy list --compartment-id "${tenancy}" --name "${NAME}" \
        --query 'data[0].id' --raw-output 2>/dev/null || true)
    if [[ -z "${policy_id}" || "${policy_id}" == "null" ]]; then
        echo "creating policy ${NAME}"
        run oci iam policy create --compartment-id "${tenancy}" --name "${NAME}" \
            --description "Kythira CI oci-monitoring job" --statements "${statements}" \
            --wait-for-state ACTIVE >/dev/null
    else
        echo "updating policy ${NAME}"
        # The CLI insists on --version-date alongside --statements; empty
        # means "evaluate against current service behaviour".
        run oci iam policy update --policy-id "${policy_id}" --statements "${statements}" \
            --version-date "" --force >/dev/null
    fi
    enable OCI
}

provision_alibaba() {
    local ok=1 s
    for s in ALIBABA_CLOUD_ACCESS_KEY_ID ALIBABA_CLOUD_ACCESS_KEY_SECRET; do
        has_secret "$s" || { echo "ERROR: secret ${s} is unset." >&2; ok=0; }
    done
    need_vars ALIBABA_CMS_REMOTE_WRITE_URL ALIBABA_CMS_PROM_QUERY_URL || ok=0
    if (( ! ok )); then
        echo "See scripts/ci-cloud-credentials/alibaba/README.md (One-time setup)." >&2
        return 1
    fi
    echo "Alibaba CloudMonitor settings: all present"
    enable ALIBABA
}

declare -A result
IFS=',' read -r -a providers <<<"${ONLY}"
for p in "${providers[@]}"; do
    case "${p}" in azure|gcp|oci|alibaba) ;; *) echo "Unknown provider: ${p}" >&2; exit 1 ;; esac
    echo
    echo "== ${p}"
    # A subshell, so one provider's failure stops only that provider. Not
    # `if ( ... )`: bash ignores set -e for anything run as a condition,
    # subshells included, so a failing step would not stop the provider.
    # The ERR trap (-E carries it into the functions) names the command that
    # stopped it, since set -e alone exits without saying why.
    set +e
    ( set -eE
      trap 'echo "ERROR: ${p} stopped at line ${LINENO} (exit $?): ${BASH_COMMAND}" >&2' ERR
      "provision_${p}" )
    rc=$?
    set -e
    if (( rc == 0 )); then result[${p}]="ready"; else result[${p}]="FAILED (see above)"; fi
done

echo
echo "== summary"
for p in "${providers[@]}"; do printf '  %-8s %s\n' "${p}" "${result[${p}]}"; done
echo
echo "The enabled jobs run on the next scheduled Real Cloud Tests run, or on a"
echo "dispatch with run_real_cloud_tests=true (Claude dispatches once you say go)."
