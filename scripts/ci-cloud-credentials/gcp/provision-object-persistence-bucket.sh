#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Creates the GCS bucket the cloud key-object persistence real tier writes to
# (.kiro/specs/cloud-object-persistence/ task 17): uniform access, public
# access prevented, soft delete off, and a lifecycle rule that expires the test
# prefix.
#
# Safe to re-run: create-if-absent, then idempotent PUTs of whole
# configuration documents.
#
# ## Two settings that are decisions, not defaults
#
# **Soft delete is disabled.** GCS enables a 7-day soft-delete retention by
# default, which bills deleted objects for a week — on a bucket whose entire
# workload is create-and-delete test objects, that is the dominant cost and it
# is invisible in a bucket listing. The design also takes "no dependence on
# provider-native versioning or soft-delete" as a non-goal, so switching it off
# keeps the bucket honest about what the engine can rely on.
#
# **Uniform bucket-level access.** ACLs are off, so access is IAM-only and
# there is one place to read the permissions from.
#
# Usage:
#   scripts/ci-cloud-credentials/gcp/provision-object-persistence-bucket.sh \
#       [--bucket NAME] [--project ID] [--location LOC] [--expiry-days N]
#       [--dry-run]
#
# If the gcloud user credential has expired but application-default
# credentials still work, this runs unchanged with:
#   CLOUDSDK_AUTH_ACCESS_TOKEN="$(gcloud auth application-default print-access-token)" \
#       scripts/ci-cloud-credentials/gcp/provision-object-persistence-bucket.sh
set -euo pipefail

BUCKET=""
PROJECT=""
LOCATION="us-central1"
EXPIRY_DAYS="7"
TEST_PREFIX="kythira-real-test/"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-object-persistence-bucket.sh [OPTIONS]

Creates (if absent) and configures the GCS bucket for the object-persistence
real tier. Safe to re-run.

Optional:
  --bucket NAME      default: kythira-ci-<project>
  --project ID       default: the gcloud config's current project
  --location LOC     default: us-central1 (matches GCP_REAL_CLOUD_TESTS_REGION)
  --expiry-days N    default: 7 — lifecycle expiry under kythira-real-test/
  --dry-run          print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bucket) BUCKET="$2"; shift 2 ;;
        --project) PROJECT="$2"; shift 2 ;;
        --location) LOCATION="$2"; shift 2 ;;
        --expiry-days) EXPIRY_DAYS="$2"; shift 2 ;;
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

: "${PROJECT:=$(gcloud config get-value project 2>/dev/null)}"
if [[ -z "${PROJECT}" || "${PROJECT}" == "(unset)" ]]; then
    echo "no project: pass --project or set one with 'gcloud config set project'" >&2
    exit 2
fi
: "${BUCKET:=kythira-ci-${PROJECT}}"

echo "project:  ${PROJECT}"
echo "bucket:   gs://${BUCKET}"
echo "location: ${LOCATION}"

LIFECYCLE_FILE="$(mktemp)"
trap 'rm -f "${LIFECYCLE_FILE}"' EXIT
cat >"${LIFECYCLE_FILE}" <<EOF
{
  "lifecycle": {
    "rule": [
      {
        "action": {"type": "Delete"},
        "condition": {"age": ${EXPIRY_DAYS}, "matchesPrefix": ["${TEST_PREFIX}"]}
      }
    ]
  }
}
EOF

# ── 1. The bucket itself ─────────────────────────────────────────────────────
if gcloud storage buckets describe "gs://${BUCKET}" --format="value(name)" >/dev/null 2>&1; then
    echo "bucket exists — leaving it alone"
else
    echo "creating bucket"
    run gcloud storage buckets create "gs://${BUCKET}" \
        --project="${PROJECT}" \
        --location="${LOCATION}" \
        --uniform-bucket-level-access \
        --public-access-prevention \
        --soft-delete-duration=0
fi

# ── 2. Re-assert the settings that matter, so a hand-edited bucket converges ──
echo "asserting access and soft-delete settings"
run gcloud storage buckets update "gs://${BUCKET}" \
    --uniform-bucket-level-access \
    --public-access-prevention \
    --clear-soft-delete

# ── 3. Lifecycle, scoped to the test prefix only ─────────────────────────────
echo "setting lifecycle expiry (${EXPIRY_DAYS} days) on ${TEST_PREFIX}"
run gcloud storage buckets update "gs://${BUCKET}" --lifecycle-file="${LIFECYCLE_FILE}"

echo
echo "done. Set the repository variable:"
echo "  GCP_OBJECT_PERSISTENCE_BUCKET=${BUCKET}"
echo
echo "  REAL_CLOUD_TESTS_GCP_OBJECT_PERSISTENCE_ENABLED=true"
echo
echo "NOT done by this script, and still required before CI can use it:"
echo "  * roles/storage.objectUser for"
echo "    ${GCP_CI_SERVICE_ACCOUNT:-the CI service account}, bound ON THIS"
echo "    BUCKET rather than at project level. Run:"
echo "      scripts/ci-cloud-credentials/gcp/provision-workload-identity.sh \\"
echo "          --project ${PROJECT} --github-org ORG --github-repo REPO \\"
echo "          --bundles <every bundle you want>,gcp-object-persistence \\"
echo "          --object-persistence-bucket ${BUCKET}"
