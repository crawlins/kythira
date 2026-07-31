#!/usr/bin/env bash
# Creates everything GitHub Actions CI needs to authenticate to Google Cloud
# via Workload Identity Federation — no service-account JSON key is ever
# generated or stored as a GitHub secret. Creates (if absent):
#   - a Workload Identity Pool and an OIDC provider trusting
#     token.actions.githubusercontent.com for this exact repository,
#   - a dedicated service account CI impersonates,
#   - the IAM role bindings for exactly the --bundles given, drawn from
#     policies/<bundle>.json.
#
# This is the GCP analogue of the aws/ directory's provision-oidc-role.sh,
# using GCP's native federation mechanism rather than a long-lived key.
#
# .kiro/specs/gcp-cloud-services/ Requirement 24.2-24.3.
#
# Usage:
#   scripts/ci-cloud-credentials/gcp/provision-workload-identity.sh \
#       --project PROJECT_ID --github-org ORG --github-repo REPO \
#       --bundles BUNDLE[,BUNDLE...] \
#       [--pool-id ID] [--provider-id ID] [--service-account-id ID] \
#       [--ref-restriction REF] [--dry-run]
#
# Bundles: gcp-quorum-manager, gcp-privateca (any non-empty subset).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
POLICY_DIR="${REPO_ROOT}/scripts/ci-cloud-credentials/gcp/policies"

PROJECT=""
GITHUB_ORG=""
GITHUB_REPO=""
BUNDLES=""
POOL_ID="kythira-ci-pool"
PROVIDER_ID="kythira-ci-github"
SA_ID="kythira-ci-real-cloud-tests"
REF_RESTRICTION=""
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-workload-identity.sh [OPTIONS]

Required:
  --project PROJECT_ID    GCP project to create the resources in
  --github-org ORG        e.g. crawlins
  --github-repo REPO      e.g. kythira
  --bundles LIST          Comma-separated: gcp-quorum-manager,gcp-privateca

Optional:
  --pool-id ID              Workload Identity Pool id (default kythira-ci-pool)
  --provider-id ID          OIDC provider id (default kythira-ci-github)
  --service-account-id ID   SA id (default kythira-ci-real-cloud-tests)
  --ref-restriction REF     Narrow trust to a single git ref (e.g. refs/heads/main)
  --dry-run                 Print the gcloud calls without running them

Safe to re-run — every step checks for existing state first, and re-running
with a different --bundles value re-applies exactly that bundle set's bindings.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) PROJECT="$2"; shift 2;;
        --github-org) GITHUB_ORG="$2"; shift 2;;
        --github-repo) GITHUB_REPO="$2"; shift 2;;
        --bundles) BUNDLES="$2"; shift 2;;
        --pool-id) POOL_ID="$2"; shift 2;;
        --provider-id) PROVIDER_ID="$2"; shift 2;;
        --service-account-id) SA_ID="$2"; shift 2;;
        --ref-restriction) REF_RESTRICTION="$2"; shift 2;;
        --dry-run) DRY_RUN=1; shift;;
        -h|--help) usage; exit 0;;
        *) echo "unknown argument: $1" >&2; usage; exit 1;;
    esac
done

[[ -n "$PROJECT" ]] || { echo "--project is required" >&2; exit 1; }
[[ -n "$GITHUB_ORG" ]] || { echo "--github-org is required" >&2; exit 1; }
[[ -n "$GITHUB_REPO" ]] || { echo "--github-repo is required" >&2; exit 1; }
[[ -n "$BUNDLES" ]] || { echo "--bundles is required" >&2; exit 1; }

run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

PROJECT_NUMBER="$(gcloud projects describe "$PROJECT" --format='value(projectNumber)')"
SA_EMAIL="${SA_ID}@${PROJECT}.iam.gserviceaccount.com"

echo "== Enabling required services =="
run gcloud services enable iamcredentials.googleapis.com compute.googleapis.com \
    privateca.googleapis.com storage.googleapis.com --project "$PROJECT"

echo "== Workload Identity Pool =="
if ! gcloud iam workload-identity-pools describe "$POOL_ID" \
        --project "$PROJECT" --location=global >/dev/null 2>&1; then
    run gcloud iam workload-identity-pools create "$POOL_ID" \
        --project "$PROJECT" --location=global \
        --display-name="kythira CI pool"
fi

echo "== OIDC provider =="
# Restrict the token subject to this repository (optionally a single ref),
# mirroring the aws role's repo:<org>/<repo>:* trust condition.
ATTR_CONDITION="assertion.repository == '${GITHUB_ORG}/${GITHUB_REPO}'"
if [[ -n "$REF_RESTRICTION" ]]; then
    ATTR_CONDITION="${ATTR_CONDITION} && assertion.ref == '${REF_RESTRICTION}'"
fi
if ! gcloud iam workload-identity-pools providers describe "$PROVIDER_ID" \
        --project "$PROJECT" --location=global \
        --workload-identity-pool="$POOL_ID" >/dev/null 2>&1; then
    run gcloud iam workload-identity-pools providers create-oidc "$PROVIDER_ID" \
        --project "$PROJECT" --location=global \
        --workload-identity-pool="$POOL_ID" \
        --display-name="GitHub Actions" \
        --issuer-uri="https://token.actions.githubusercontent.com" \
        --attribute-mapping="google.subject=assertion.sub,attribute.repository=assertion.repository,attribute.ref=assertion.ref" \
        --attribute-condition="$ATTR_CONDITION"
fi

echo "== Service account =="
if ! gcloud iam service-accounts describe "$SA_EMAIL" --project "$PROJECT" >/dev/null 2>&1; then
    run gcloud iam service-accounts create "$SA_ID" \
        --project "$PROJECT" --display-name="kythira CI real-cloud-tests"
fi

echo "== Allow the pool's principals for this repo to impersonate the SA =="
MEMBER="principalSet://iam.googleapis.com/projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}/attribute.repository/${GITHUB_ORG}/${GITHUB_REPO}"
run gcloud iam service-accounts add-iam-policy-binding "$SA_EMAIL" \
    --project "$PROJECT" \
    --role="roles/iam.workloadIdentityUser" \
    --member="$MEMBER"

echo "== Project role bindings for the selected bundles =="
IFS=',' read -ra BUNDLE_ARR <<< "$BUNDLES"
for bundle in "${BUNDLE_ARR[@]}"; do
    policy="${POLICY_DIR}/${bundle}.json"
    [[ -f "$policy" ]] || { echo "unknown bundle '$bundle' (no ${policy})" >&2; exit 1; }
    echo "-- ${bundle} --"
    while IFS= read -r role; do
        [[ -n "$role" ]] || continue
        run gcloud projects add-iam-policy-binding "$PROJECT" \
            --member="serviceAccount:${SA_EMAIL}" \
            --role="$role" --condition=None
    done < <(python3 -c 'import json,sys; [print(e["role"]) for e in json.load(open(sys.argv[1]))]' "$policy")
done

PROVIDER_RESOURCE="projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}/providers/${PROVIDER_ID}"

cat <<EOF

== Done ==

Set these repository variables (values consumed by the gcp job in
.github/workflows/real-cloud-tests.yml):

  gh variable set GCP_CI_WORKLOAD_IDENTITY_PROVIDER --body "${PROVIDER_RESOURCE}"
  gh variable set GCP_CI_SERVICE_ACCOUNT           --body "${SA_EMAIL}"
  gh variable set GCP_REAL_CLOUD_TESTS_PROJECT     --body "${PROJECT}"
  # optional (defaults to us-central1):
  # gh variable set GCP_REAL_CLOUD_TESTS_REGION    --body "us-central1"

Then enable the toggles you want:

  gh variable set REAL_CLOUD_TESTS_ENABLED                       --body "true"
  gh variable set REAL_CLOUD_TESTS_GCP_ENABLED                   --body "true"
  gh variable set REAL_CLOUD_TESTS_GCP_QUORUM_MANAGER_ENABLED    --body "true"
  gh variable set REAL_CLOUD_TESTS_GCP_PRIVATECA_ENABLED         --body "true"
EOF
