#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time GCP setup for the real-cloud OIDC federation run
# (.kiro/specs/iam-compatible-auth/ task 5, .github/workflows/oidc-federation.yml).
# Run once, locally, as a project owner: the CI service account may not
# create workload identity pools.
#
# Creates (if absent):
#   1. A workload identity pool and an OIDC provider trusting the Kythira
#      issuer (the S3 issuer URL the AWS script printed). google.subject is
#      the token's sub, and the provider only accepts sub ==
#      workload:<subject-uri>. Its allowed audience is the default, the
#      provider's own resource name, which is what the test requests.
#   2. A bucket holding only the test object, test-object/federation.txt:
#      uniform access, public access prevented, soft delete off.
#   3. roles/storage.objectViewer on that bucket for the federated principal
#      principal://.../subject/workload:<subject-uri>. Direct federated
#      access: no service account and no impersonation.
#
# Pools and providers are free; the bucket holds a few bytes. Resources are
# labelled kythira-test=oidc-federation where GCP allows labels. Safe to
# re-run.
#
# Usage:
#   scripts/ci-cloud-credentials/gcp/provision-oidc-federation-test.sh \
#       --project PROJECT_ID --issuer-url URL \
#       [--subject-uri URI] [--pool-id ID] [--provider-id ID] \
#       [--bucket NAME] [--location LOC] [--delete] [--dry-run]
set -euo pipefail

PROJECT=""
ISSUER_URL=""
SUBJECT_URI="spiffe://kythira.test/wl/oidc-federation"
POOL_ID="kythira-oidc-fed"
PROVIDER_ID="kythira-ca"
BUCKET=""
LOCATION="us-central1"
TEST_KEY="test-object/federation.txt"
DELETE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-oidc-federation-test.sh [OPTIONS]

One-time GCP setup for .github/workflows/oidc-federation.yml. Run as a
project owner. Safe to re-run.

Required:
  --project PROJECT_ID
  --issuer-url URL      the issuer URL the AWS script printed,
                        https://<bucket>.s3.<region>.amazonaws.com/<prefix>

Optional:
  --subject-uri URI     default: spiffe://kythira.test/wl/oidc-federation
  --pool-id ID          default: kythira-oidc-fed
  --provider-id ID      default: kythira-ca
  --bucket NAME         default: kythira-oidc-fed-<project>
  --location LOC        default: us-central1
  --delete              remove the binding, the bucket, the provider and pool
  --dry-run             print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) PROJECT="$2"; shift 2 ;;
        --issuer-url) ISSUER_URL="$2"; shift 2 ;;
        --subject-uri) SUBJECT_URI="$2"; shift 2 ;;
        --pool-id) POOL_ID="$2"; shift 2 ;;
        --provider-id) PROVIDER_ID="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --location) LOCATION="$2"; shift 2 ;;
        --delete) DELETE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${PROJECT}" || ( "${DELETE}" -eq 0 && -z "${ISSUER_URL}" ) ]]; then
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

: "${BUCKET:=kythira-oidc-fed-${PROJECT}}"
PROJECT_NUMBER="$(gcloud projects describe "${PROJECT}" --format='value(projectNumber)')"
POOL_NAME="projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}"
PROVIDER_RESOURCE="//iam.googleapis.com/${POOL_NAME}/providers/${PROVIDER_ID}"
PRINCIPAL="principal://iam.googleapis.com/${POOL_NAME}/subject/workload:${SUBJECT_URI}"

echo "project:   ${PROJECT} (${PROJECT_NUMBER})"
echo "issuer:    ${ISSUER_URL}"
echo "provider:  ${PROVIDER_RESOURCE}"
echo "principal: ${PRINCIPAL}"
echo "bucket:    gs://${BUCKET}"

if [[ "${DELETE}" -eq 1 ]]; then
    run gcloud storage rm --recursive "gs://${BUCKET}" --project "${PROJECT}" || true
    run gcloud iam workload-identity-pools providers delete "${PROVIDER_ID}" \
        --project "${PROJECT}" --location global --workload-identity-pool "${POOL_ID}" --quiet || true
    run gcloud iam workload-identity-pools delete "${POOL_ID}" \
        --project "${PROJECT}" --location global --quiet || true
    echo "done (deleted pools stay recoverable for 30 days, and their ids reserved)"
    exit 0
fi

# ── 1. Pool and provider ─────────────────────────────────────────────────────
if gcloud iam workload-identity-pools describe "${POOL_ID}" \
        --project "${PROJECT}" --location global >/dev/null 2>&1; then
    echo "pool exists"
else
    run gcloud iam workload-identity-pools create "${POOL_ID}" \
        --project "${PROJECT}" --location global \
        --display-name "Kythira OIDC federation test" \
        --description "kythira-test=oidc-federation: trusts the Kythira CA issuer"
fi

CONDITION="assertion.sub == 'workload:${SUBJECT_URI}'"
if gcloud iam workload-identity-pools providers describe "${PROVIDER_ID}" \
        --project "${PROJECT}" --location global --workload-identity-pool "${POOL_ID}" \
        >/dev/null 2>&1; then
    echo "provider exists; updating issuer and condition"
    run gcloud iam workload-identity-pools providers update-oidc "${PROVIDER_ID}" \
        --project "${PROJECT}" --location global --workload-identity-pool "${POOL_ID}" \
        --issuer-uri "${ISSUER_URL}" \
        --attribute-mapping "google.subject=assertion.sub" \
        --attribute-condition "${CONDITION}"
else
    run gcloud iam workload-identity-pools providers create-oidc "${PROVIDER_ID}" \
        --project "${PROJECT}" --location global --workload-identity-pool "${POOL_ID}" \
        --issuer-uri "${ISSUER_URL}" \
        --attribute-mapping "google.subject=assertion.sub" \
        --attribute-condition "${CONDITION}"
fi

# ── 2. Bucket and test object ────────────────────────────────────────────────
if gcloud storage buckets describe "gs://${BUCKET}" --project "${PROJECT}" >/dev/null 2>&1; then
    echo "bucket exists"
else
    run gcloud storage buckets create "gs://${BUCKET}" --project "${PROJECT}" \
        --location "${LOCATION}" --uniform-bucket-level-access \
        --public-access-prevention --soft-delete-duration=0
fi
run gcloud storage buckets update "gs://${BUCKET}" --project "${PROJECT}" \
    --update-labels kythira-test=oidc-federation
tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
printf 'kythira-oidc-federation-test\n' > "${tmp}"
run gcloud storage cp "${tmp}" "gs://${BUCKET}/${TEST_KEY}" --project "${PROJECT}" \
    --content-type text/plain

# ── 3. The federated principal may read the bucket's objects ─────────────────
run gcloud storage buckets add-iam-policy-binding "gs://${BUCKET}" --project "${PROJECT}" \
    --member "${PRINCIPAL}" --role roles/storage.objectViewer >/dev/null

echo
echo "done. Set these repository variables:"
echo "  OIDC_FED_GCP_PROVIDER=${PROVIDER_RESOURCE}"
echo "  OIDC_FED_GCS_BUCKET=${BUCKET}"
