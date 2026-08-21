#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Provisions the Azure AD App Registration + Federated Identity Credential
# (Workload Identity Federation — no client secret, mirroring the AWS
# provisioner's OIDC role) that
# .github/workflows/real-cloud-tests.yml's `azure` job authenticates as via
# `azure/login@v2`, plus the RBAC role assignments for whichever bundles are
# requested. Run locally by an operator with Azure AD Application
# Administrator + User Access Administrator (or Owner) rights — never in CI.
#
# Unlike the AWS provisioner, no custom IAM-policy-equivalent JSON is needed:
# the two bundles below map onto Azure's own built-in RBAC roles (Virtual
# Machine Contributor, Network Contributor, Key Vault Crypto User), so
# policies/*.json here are role-assignment *fragments* (role name + scope),
# not custom role definitions.
#
# Usage:
#   provision-federated-identity.sh \
#       --github-org <org> --github-repo <repo> \
#       --subscription-id <sub> --resource-group <rg> \
#       --bundles quorum-manager,key-vault,object-persistence \
#       [--key-vault-name <vault>] \
#       [--storage-account <acct>] [--storage-container <name>] \
#       [--storage-resource-group <rg>] \
#       [--dry-run]
#
# The object-persistence bundle's three --storage-* options default to what
# provision-object-persistence-container.sh creates by default, and the
# resolved values are echoed before the assignment is made — the scope is a
# container path, and a typo in it produces a role assignment that succeeds
# and grants nothing.

set -euo pipefail

GITHUB_ORG=""
GITHUB_REPO=""
SUBSCRIPTION_ID=""
RESOURCE_GROUP=""
BUNDLES=""
KEY_VAULT_NAME=""
STORAGE_ACCOUNT="kythirarealtestobj"
STORAGE_CONTAINER="kythira-raft"
STORAGE_RESOURCE_GROUP=""
DRY_RUN=0
APP_NAME="kythira-ci-real-cloud-tests"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --github-org) GITHUB_ORG="$2"; shift 2 ;;
        --github-repo) GITHUB_REPO="$2"; shift 2 ;;
        --subscription-id) SUBSCRIPTION_ID="$2"; shift 2 ;;
        --resource-group) RESOURCE_GROUP="$2"; shift 2 ;;
        --bundles) BUNDLES="$2"; shift 2 ;;
        --key-vault-name) KEY_VAULT_NAME="$2"; shift 2 ;;
        --storage-account) STORAGE_ACCOUNT="$2"; shift 2 ;;
        --storage-container) STORAGE_CONTAINER="$2"; shift 2 ;;
        --storage-resource-group) STORAGE_RESOURCE_GROUP="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

for required in GITHUB_ORG GITHUB_REPO SUBSCRIPTION_ID RESOURCE_GROUP BUNDLES; do
    if [[ -z "${!required}" ]]; then
        echo "Missing required --${required,,} (see script header for usage)" >&2
        exit 1
    fi
done

# The storage account need not live in the same resource group as the VM/VMSS
# test resources — provision-object-persistence-container.sh creates it in
# kythira-realtest-rg by default, which happens to be the same group today, but
# nothing enforces that and a wrong group here yields a scope that names a
# resource which does not exist.
: "${STORAGE_RESOURCE_GROUP:=${RESOURCE_GROUP}}"

run() {
    echo "+ $*"
    if [[ "$DRY_RUN" -eq 0 ]]; then
        "$@"
    fi
}

echo "== Azure AD App Registration =="
APP_ID=$(az ad app list --display-name "$APP_NAME" --query '[0].appId' -o tsv 2>/dev/null || true)
if [[ -z "$APP_ID" || "$APP_ID" == "null" ]]; then
    echo "Creating app registration '$APP_NAME'..."
    run az ad app create --display-name "$APP_NAME"
    APP_ID=$(az ad app list --display-name "$APP_NAME" --query '[0].appId' -o tsv)
else
    echo "App registration '$APP_NAME' already exists (appId=$APP_ID)"
fi

echo "== Service principal =="
SP_ID=$(az ad sp list --filter "appId eq '$APP_ID'" --query '[0].id' -o tsv 2>/dev/null || true)
if [[ -z "$SP_ID" || "$SP_ID" == "null" ]]; then
    run az ad sp create --id "$APP_ID"
    SP_ID=$(az ad sp list --filter "appId eq '$APP_ID'" --query '[0].id' -o tsv)
else
    echo "Service principal already exists (id=$SP_ID)"
fi

echo "== Federated identity credential (GitHub OIDC, no client secret) =="
# real-cloud-tests.yml's azure job sets `environment: real-cloud-tests`
# (needed for the environment's own required-reviewers/secrets scoping), which
# changes the OIDC subject GitHub issues to
# repo:<org>/<repo>:environment:<environment-name> -- unlike a plain
# ref-scoped subject (repo:<org>/<repo>:ref:refs/heads/main), this is NOT
# branch-specific, so it authenticates from any branch/PR that runs a job
# targeting this environment (matches the AWS provisioner's
# repo:<org>/<repo>:* trust policy's own "any branch/tag/PR" intent, just
# scoped by environment name instead of by wildcard ref, since Azure AD
# federated credentials require an exact subject match with no wildcard
# support the way an AWS IAM trust policy's StringLike condition has).
# Narrow this further (e.g. to a specific branch) if your threat model wants
# it -- but note a ref-scoped subject would then need updating every time
# this environment's jobs are exercised from a different branch, which is
# exactly the mismatch this comment replaces.
FIC_NAME="github-actions-real-cloud-tests"
EXISTING_FIC=$(az ad app federated-credential list --id "$APP_ID" \
    --query "[?name=='$FIC_NAME'].name" -o tsv 2>/dev/null || true)
if [[ -z "$EXISTING_FIC" ]]; then
    run az ad app federated-credential create --id "$APP_ID" --parameters "$(cat <<JSON
{
  "name": "$FIC_NAME",
  "issuer": "https://token.actions.githubusercontent.com",
  "subject": "repo:${GITHUB_ORG}/${GITHUB_REPO}:environment:real-cloud-tests",
  "audiences": ["api://AzureADTokenExchange"]
}
JSON
)"
else
    echo "Federated credential '$FIC_NAME' already exists"
fi

echo "== Role assignments =="
IFS=',' read -ra BUNDLE_LIST <<< "$BUNDLES"
for bundle in "${BUNDLE_LIST[@]}"; do
    policy_file="$(dirname "$0")/policies/${bundle}.json"
    if [[ ! -f "$policy_file" ]]; then
        echo "No policy fragment for bundle '$bundle' at $policy_file" >&2
        exit 1
    fi
    echo "-- bundle: $bundle --" >&2
    # python3 (no third-party packages) does the {SUBSCRIPTION_ID}/{RESOURCE_GROUP}/
    # {VAULT_NAME} substitution and JSON iteration, mirroring the AWS
    # provisioner's use of python3 to merge bundle policy JSON.
    python3 - "$policy_file" "$SUBSCRIPTION_ID" "$RESOURCE_GROUP" "$KEY_VAULT_NAME" \
              "$STORAGE_RESOURCE_GROUP" "$STORAGE_ACCOUNT" "$STORAGE_CONTAINER" <<'PYEOF'
import json, sys
policy_file, sub, rg, vault, storage_rg, storage_acct, storage_container = sys.argv[1:8]
with open(policy_file) as f:
    fragments = json.load(f)
for frag in fragments:
    scope = (frag["scope"]
             .replace("{SUBSCRIPTION_ID}", sub)
             .replace("{RESOURCE_GROUP}", rg)
             .replace("{VAULT_NAME}", vault)
             .replace("{STORAGE_RESOURCE_GROUP}", storage_rg)
             .replace("{STORAGE_ACCOUNT}", storage_acct)
             .replace("{STORAGE_CONTAINER}", storage_container))
    # An unsubstituted placeholder is a scope that will be accepted by nobody
    # and, worse, could be accepted by az as a literal name. Refuse rather
    # than assign it.
    if "{" in scope:
        sys.exit(f"unsubstituted placeholder in scope: {scope}")
    print(f'{frag["roleDefinitionName"]}\t{scope}')
PYEOF
done | while IFS=$'\t' read -r role scope; do
    run az role assignment create --assignee "$APP_ID" --role "$role" --scope "$scope"
done

echo
echo "Done. Set these repository variables (gh CLI shown; adjust as needed):"
echo "  gh variable set AZURE_CI_CLIENT_ID --body \"$APP_ID\""
echo "  gh variable set AZURE_CI_TENANT_ID --body \"\$(az account show --query tenantId -o tsv)\""
echo "  gh variable set AZURE_CI_SUBSCRIPTION_ID --body \"$SUBSCRIPTION_ID\""
echo "  gh variable set REAL_CLOUD_TESTS_AZURE_ENABLED --body true"
if [[ ",${BUNDLES}," == *",object-persistence,"* ]]; then
    echo "  gh variable set AZURE_OBJECT_PERSISTENCE_ACCOUNT --body \"$STORAGE_ACCOUNT\""
    echo "  gh variable set AZURE_OBJECT_PERSISTENCE_CONTAINER --body \"$STORAGE_CONTAINER\""
    echo "  gh variable set REAL_CLOUD_TESTS_AZURE_OBJECT_PERSISTENCE_ENABLED --body true"
fi
