#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time Azure setup for the real-cloud OIDC federation run
# (.kiro/specs/iam-compatible-auth/ task 5, .github/workflows/oidc-federation.yml).
# Run once, locally, as someone who may create app registrations (Application
# Administrator) and assign roles on the storage account (User Access
# Administrator or Owner). The CI identity may do neither.
#
# Creates (if absent):
#   1. An app registration and its service principal,
#      kythira-oidc-federation-test, with no secret and no certificate.
#   2. A federated identity credential on it trusting the Kythira issuer (the
#      S3 issuer URL the AWS script printed) for exactly one subject,
#      workload:<subject-uri>, with the audience api://AzureADTokenExchange.
#      This is what azure_federated_source presents as its client_assertion.
#   3. A private container in an existing storage account holding only the
#      test object, test-object/federation.txt.
#   4. Storage Blob Data Reader for the service principal on that container
#      alone.
#
# App registrations and role assignments are free; the blob is a few bytes.
# Safe to re-run.
#
# Usage:
#   scripts/ci-cloud-credentials/azure/provision-oidc-federation-test.sh \
#       --subscription-id SUB --resource-group RG --storage-account ACCT \
#       --issuer-url URL [--subject-uri URI] [--container NAME] \
#       [--app-name NAME] [--delete] [--dry-run]
set -euo pipefail

SUBSCRIPTION_ID=""
RESOURCE_GROUP=""
STORAGE_ACCOUNT=""
ISSUER_URL=""
SUBJECT_URI="spiffe://kythira.test/wl/oidc-federation"
CONTAINER="kythira-oidc-fed"
APP_NAME="kythira-oidc-federation-test"
FIC_NAME="kythira-ca-issuer"
TEST_KEY="test-object/federation.txt"
DELETE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-oidc-federation-test.sh [OPTIONS]

One-time Azure setup for .github/workflows/oidc-federation.yml. Run as an
Application Administrator with User Access Administrator on the storage
account. Safe to re-run.

Required:
  --subscription-id SUB
  --resource-group RG   the storage account's resource group
  --storage-account ACCT
                        an existing account; the script adds one container
  --issuer-url URL      the issuer URL the AWS script printed,
                        https://<bucket>.s3.<region>.amazonaws.com/<prefix>

Optional:
  --subject-uri URI     default: spiffe://kythira.test/wl/oidc-federation
  --container NAME      default: kythira-oidc-fed
  --app-name NAME       default: kythira-oidc-federation-test
  --delete              remove the role assignment, container and app
  --dry-run             print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --subscription-id) SUBSCRIPTION_ID="$2"; shift 2 ;;
        --resource-group) RESOURCE_GROUP="$2"; shift 2 ;;
        --storage-account) STORAGE_ACCOUNT="$2"; shift 2 ;;
        --issuer-url) ISSUER_URL="$2"; shift 2 ;;
        --subject-uri) SUBJECT_URI="$2"; shift 2 ;;
        --container) CONTAINER="$2"; shift 2 ;;
        --app-name) APP_NAME="$2"; shift 2 ;;
        --delete) DELETE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${SUBSCRIPTION_ID}" || -z "${RESOURCE_GROUP}" || -z "${STORAGE_ACCOUNT}" ||
      ( "${DELETE}" -eq 0 && -z "${ISSUER_URL}" ) ]]; then
    usage >&2
    exit 2
fi

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        { printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'; } >&2
        return 0
    fi
    "$@"
}

az account set --subscription "${SUBSCRIPTION_ID}"
TENANT_ID="$(az account show --query tenantId -o tsv)"
SCOPE="/subscriptions/${SUBSCRIPTION_ID}/resourceGroups/${RESOURCE_GROUP}/providers/Microsoft.Storage/storageAccounts/${STORAGE_ACCOUNT}/blobServices/default/containers/${CONTAINER}"
SUBJECT="workload:${SUBJECT_URI}"

echo "tenant:    ${TENANT_ID}"
echo "issuer:    ${ISSUER_URL}"
echo "subject:   ${SUBJECT}"
echo "container: ${STORAGE_ACCOUNT}/${CONTAINER}"

APP_ID="$(az ad app list --display-name "${APP_NAME}" --query '[0].appId' -o tsv 2>/dev/null || true)"
[[ "${APP_ID}" == "null" ]] && APP_ID=""

if [[ "${DELETE}" -eq 1 ]]; then
    if [[ -n "${APP_ID}" ]]; then
        run az role assignment delete --assignee "${APP_ID}" --scope "${SCOPE}" || true
        run az ad app delete --id "${APP_ID}" || true
    fi
    run az storage container delete --account-name "${STORAGE_ACCOUNT}" --name "${CONTAINER}" \
        --auth-mode login || true
    echo "done (a deleted app registration stays restorable for 30 days)"
    exit 0
fi

# ── 1. App registration and service principal ────────────────────────────────
if [[ -z "${APP_ID}" ]]; then
    run az ad app create --display-name "${APP_NAME}" --sign-in-audience AzureADMyOrg >/dev/null
    APP_ID="$(az ad app list --display-name "${APP_NAME}" --query '[0].appId' -o tsv)"
else
    echo "app registration exists (appId ${APP_ID})"
fi
if [[ -z "$(az ad sp list --filter "appId eq '${APP_ID}'" --query '[0].id' -o tsv 2>/dev/null)" ]]; then
    run az ad sp create --id "${APP_ID}" >/dev/null
else
    echo "service principal exists"
fi

# ── 2. Federated identity credential ─────────────────────────────────────────
# Entra compares issuer, subject and audience exactly. The issuer must match
# the token's iss character for character, so no trailing slash.
FIC="$(python3 - "${FIC_NAME}" "${ISSUER_URL}" "${SUBJECT}" <<'PY'
import json, sys
name, issuer, subject = sys.argv[1:4]
print(json.dumps({"name": name, "issuer": issuer, "subject": subject,
                  "description": "kythira-test=oidc-federation",
                  "audiences": ["api://AzureADTokenExchange"]}))
PY
)"
if az ad app federated-credential show --id "${APP_ID}" --federated-credential-id "${FIC_NAME}" \
        >/dev/null 2>&1; then
    echo "federated credential exists; updating issuer and subject"
    run az ad app federated-credential update --id "${APP_ID}" \
        --federated-credential-id "${FIC_NAME}" --parameters "${FIC}"
else
    run az ad app federated-credential create --id "${APP_ID}" --parameters "${FIC}" >/dev/null
fi

# ── 3. Container and test object ─────────────────────────────────────────────
# --auth-mode login: the operator's own Entra identity, so no account key is
# read. The operator needs a data-plane role (Storage Blob Data Contributor)
# on the account for the upload.
if [[ "$(az storage container exists --account-name "${STORAGE_ACCOUNT}" --name "${CONTAINER}" \
           --auth-mode login --query exists -o tsv)" == "true" ]]; then
    echo "container exists"
else
    run az storage container create --account-name "${STORAGE_ACCOUNT}" --name "${CONTAINER}" \
        --public-access off --auth-mode login \
        --metadata kythira_test=oidc-federation >/dev/null
fi
tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
printf 'kythira-oidc-federation-test\n' > "${tmp}"
run az storage blob upload --account-name "${STORAGE_ACCOUNT}" --container-name "${CONTAINER}" \
    --name "${TEST_KEY}" --file "${tmp}" --content-type text/plain --overwrite \
    --auth-mode login >/dev/null

# ── 4. Read access on that container only ────────────────────────────────────
if [[ -n "$(az role assignment list --assignee "${APP_ID}" --scope "${SCOPE}" \
               --role "Storage Blob Data Reader" --query '[0].id' -o tsv 2>/dev/null)" ]]; then
    echo "role assignment exists"
else
    run az role assignment create --assignee "${APP_ID}" --role "Storage Blob Data Reader" \
        --scope "${SCOPE}" >/dev/null
fi

echo
echo "done. Set these repository variables (role assignments can take a few"
echo "minutes to apply):"
echo "  OIDC_FED_AZURE_TENANT_ID=${TENANT_ID}"
echo "  OIDC_FED_AZURE_CLIENT_ID=${APP_ID}"
echo "  OIDC_FED_AZURE_STORAGE_ACCOUNT=${STORAGE_ACCOUNT}"
echo "  OIDC_FED_AZURE_CONTAINER=${CONTAINER}"
