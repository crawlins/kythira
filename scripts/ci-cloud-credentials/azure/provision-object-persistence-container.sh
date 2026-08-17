#!/usr/bin/env bash
# Creates the Azure Blob storage account and container the cloud key-object
# persistence real tier writes to (.kiro/specs/cloud-object-persistence/
# task 17): private, TLS 1.2, and **ZRS** — see the durability note below.
#
# ## Why Standard_ZRS and not Standard_LRS or _GRS
#
# Azure is the one provider in this spec whose "a 2xx write is durable" claim
# is *account configuration the engine does not control* (design.md's N1 row).
# ZRS is the only redundancy mode Microsoft documents as writing synchronously
# to all three zone replicas before returning success, which is what the
# engine's fsync-equivalence argument needs. LRS is acceptable but is
# single-datacenter durability; GRS's cross-region copy is **asynchronous**, so
# a 2xx there means durable in the primary region only. The engine will not
# read or enforce this setting — it is an operator prerequisite, and this
# script is where the prerequisite is met.
#
# Safe to re-run: every step is create-if-absent.
#
# Usage:
#   scripts/ci-cloud-credentials/azure/provision-object-persistence-container.sh \
#       [--account NAME] [--resource-group RG] [--location LOC] \
#       [--container NAME] [--sku SKU] [--dry-run]
set -euo pipefail

ACCOUNT="kythirarealtestobj"
RESOURCE_GROUP="kythira-realtest-rg"
LOCATION="eastus"
CONTAINER="kythira-raft"
SKU="Standard_ZRS"
GRANT_CALLER_DATA_ROLE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-object-persistence-container.sh [OPTIONS]

Creates (if absent) the storage account and blob container for the
object-persistence real tier. Safe to re-run.

Optional:
  --account NAME         default: kythirarealtestobj (3-24 lowercase
                          alphanumeric, globally unique)
  --resource-group RG    default: kythira-realtest-rg (the group the other
                          Azure real-test resources already live in)
  --location LOC         default: eastus (matches AZURE_TEST_LOCATION)
  --container NAME       default: kythira-raft
  --sku SKU              default: Standard_ZRS — read the header before
                          changing this; it is a durability decision
  --grant-caller-data-role
                         assign 'Storage Blob Data Contributor' on this
                          account to the signed-in principal if the data-plane
                          probe fails. Owner does NOT include blob-data
                          access, so a fresh account needs this once.
  --dry-run              print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --account) ACCOUNT="$2"; shift 2 ;;
        --resource-group) RESOURCE_GROUP="$2"; shift 2 ;;
        --location) LOCATION="$2"; shift 2 ;;
        --container) CONTAINER="$2"; shift 2 ;;
        --sku) SKU="$2"; shift 2 ;;
        --grant-caller-data-role) GRANT_CALLER_DATA_ROLE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'
        return 0
    fi
    "$@"
}

SUBSCRIPTION="$(az account show --query id -o tsv)"
echo "subscription:   ${SUBSCRIPTION}"
echo "resource group: ${RESOURCE_GROUP} (${LOCATION})"
echo "account:        ${ACCOUNT}  sku=${SKU}"
echo "container:      ${CONTAINER}"

# ── 0. The resource provider ─────────────────────────────────────────────────
#
# A subscription that has never created a storage account has Microsoft.Storage
# unregistered, and every storage call then fails with the actively misleading
# "SubscriptionNotFound" — which reads like the subscription is gone rather
# than like a provider needs registering. Checked here so the next person does
# not spend the same twenty minutes.
STATE="$(az provider show -n Microsoft.Storage --query registrationState -o tsv 2>/dev/null || echo Unknown)"
if [[ "${STATE}" != "Registered" ]]; then
    echo "Microsoft.Storage is ${STATE} — registering (one-time, free)"
    run az provider register -n Microsoft.Storage
    if [[ "${DRY_RUN}" -eq 0 ]]; then
        for _ in $(seq 1 60); do
            STATE="$(az provider show -n Microsoft.Storage --query registrationState -o tsv)"
            [[ "${STATE}" == "Registered" ]] && break
            sleep 10
        done
        if [[ "${STATE}" != "Registered" ]]; then
            echo "Microsoft.Storage still ${STATE} after 10 minutes — rerun later" >&2
            exit 1
        fi
    fi
fi

# ── 1. Resource group ────────────────────────────────────────────────────────
if az group show -n "${RESOURCE_GROUP}" >/dev/null 2>&1; then
    echo "resource group exists — leaving it alone"
else
    run az group create -n "${RESOURCE_GROUP}" -l "${LOCATION}" -o none
fi

# ── 2. Storage account ───────────────────────────────────────────────────────
if az storage account show -n "${ACCOUNT}" -g "${RESOURCE_GROUP}" >/dev/null 2>&1; then
    echo "storage account exists — leaving it alone"
    EXISTING_SKU="$(az storage account show -n "${ACCOUNT}" -g "${RESOURCE_GROUP}" \
        --query sku.name -o tsv)"
    if [[ "${EXISTING_SKU}" != "${SKU}" ]]; then
        echo "WARNING: existing account is ${EXISTING_SKU}, not ${SKU} — the" >&2
        echo "         durability argument in this script's header assumes ZRS." >&2
    fi
else
    echo "creating storage account"
    run az storage account create \
        --name "${ACCOUNT}" \
        --resource-group "${RESOURCE_GROUP}" \
        --location "${LOCATION}" \
        --sku "${SKU}" \
        --kind StorageV2 \
        --access-tier Hot \
        --min-tls-version TLS1_2 \
        --https-only true \
        --allow-blob-public-access false \
        -o none
fi

# ── 3. Container ─────────────────────────────────────────────────────────────
#
# Container creation succeeds for an Owner **without any blob-data RBAC** —
# containers are management-plane resources. So container creation proves
# nothing about whether anything can read or write a blob, and step 4 exists
# because this step's success is not the evidence it looks like. (Measured:
# `container create` and `container list` both succeeded while every blob
# operation returned "You do not have the required permissions".)
ACCOUNT_SCOPE="/subscriptions/${SUBSCRIPTION}/resourceGroups/${RESOURCE_GROUP}/providers/Microsoft.Storage/storageAccounts/${ACCOUNT}"
if [[ "${DRY_RUN}" -eq 1 ]]; then
    run az storage container create --name "${CONTAINER}" --account-name "${ACCOUNT}" \
        --auth-mode login
elif az resource show --ids "${ACCOUNT_SCOPE}/blobServices/default/containers/${CONTAINER}" \
        >/dev/null 2>&1; then
    echo "container exists — leaving it alone"
else
    echo "creating container"
    run az storage container create --name "${CONTAINER}" --account-name "${ACCOUNT}" \
        --auth-mode login -o none
fi

# ── 4. Prove the data plane actually works ───────────────────────────────────
#
# Owner is a management-plane role: it can create the account and the
# container and still not write a byte. The engine authenticates with an AAD
# bearer token against the data plane, so that is what has to be proven — with
# a real round trip, not an inference from step 3.
if [[ "${DRY_RUN}" -eq 0 ]]; then
    echo "probing the data plane (write, read back, delete)"
    PROBE_FILE="$(mktemp)"
    PROBE_BLOB="kythira-real-test/.provisioning-probe"
    trap 'rm -f "${PROBE_FILE}"' EXIT
    printf 'provisioning probe\n' >"${PROBE_FILE}"

    if ! az storage blob upload --account-name "${ACCOUNT}" --auth-mode login \
            -c "${CONTAINER}" -n "${PROBE_BLOB}" -f "${PROBE_FILE}" --overwrite -o none \
            2>/dev/null; then
        if [[ "${GRANT_CALLER_DATA_ROLE}" -eq 1 ]]; then
            CALLER_OID="$(az ad signed-in-user show --query id -o tsv)"
            echo "granting 'Storage Blob Data Contributor' to the caller on this account"
            az role assignment create --role "Storage Blob Data Contributor" \
                --assignee-object-id "${CALLER_OID}" --assignee-principal-type User \
                --scope "${ACCOUNT_SCOPE}" -o none
            # RBAC is eventually consistent; it took ~45 s when this was written.
            echo "waiting for the role assignment to take effect"
            for _ in $(seq 1 20); do
                az storage blob upload --account-name "${ACCOUNT}" --auth-mode login \
                    -c "${CONTAINER}" -n "${PROBE_BLOB}" -f "${PROBE_FILE}" --overwrite \
                    -o none 2>/dev/null && break
                sleep 15
            done
        fi
    fi

    if az storage blob download --account-name "${ACCOUNT}" --auth-mode login \
            -c "${CONTAINER}" -n "${PROBE_BLOB}" -f /dev/null --overwrite -o none 2>/dev/null; then
        az storage blob delete --account-name "${ACCOUNT}" --auth-mode login \
            -c "${CONTAINER}" -n "${PROBE_BLOB}" -o none 2>/dev/null || true
        echo "data plane OK"
    else
        echo >&2
        echo "DATA PLANE UNUSABLE: the container exists but blobs cannot be written." >&2
        echo "Owner does not grant blob-data access. Assign the data role with:" >&2
        echo "  az role assignment create --role 'Storage Blob Data Contributor' \\" >&2
        echo "    --assignee-object-id \$(az ad signed-in-user show --query id -o tsv) \\" >&2
        echo "    --assignee-principal-type User --scope ${ACCOUNT_SCOPE}" >&2
        echo "or re-run this script with --grant-caller-data-role." >&2
        exit 1
    fi
fi

echo
echo "done. Set the repository variables:"
echo "  AZURE_OBJECT_PERSISTENCE_ACCOUNT=${ACCOUNT}"
echo "  AZURE_OBJECT_PERSISTENCE_CONTAINER=${CONTAINER}"
echo
echo "NOT done by this script, and still required before CI can use it:"
echo "  * 'Storage Blob Data Contributor' on this account for the CI federated"
echo "    identity (AZURE_CI_CLIENT_ID), scoped to the account and no wider."
