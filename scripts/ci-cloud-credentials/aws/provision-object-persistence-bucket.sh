#!/usr/bin/env bash
# Creates the S3 bucket the cloud key-object persistence real tier writes to
# (.kiro/specs/cloud-object-persistence/ task 17): private, encrypted, with a
# lifecycle rule that expires the test prefix so a shared CI bucket cannot
# accumulate state or cost.
#
# Buckets are operator-owned prerequisites in every provider spec in this tree
# — the engine reads and writes objects, it never administers storage — so
# this script exists precisely because the engine will not do any of it.
#
# Safe to re-run: every step is a create-if-absent or an idempotent PUT of a
# whole configuration document. Running it twice changes nothing.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-object-persistence-bucket.sh \
#       [--bucket NAME] [--region REGION] [--profile PROFILE] \
#       [--expiry-days N] [--dry-run]
set -euo pipefail

BUCKET=""
REGION="us-east-1"
PROFILE=""
EXPIRY_DAYS="7"
TEST_PREFIX="kythira-real-test/"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-object-persistence-bucket.sh [OPTIONS]

Creates (if absent) and configures the S3 bucket for the object-persistence
real tier. Safe to re-run.

Optional:
  --bucket NAME        default: kythira-ci-<account-id>
  --region REGION      default: us-east-1 (matches real-cloud-tests.yml's
                        AWS_REAL_CLOUD_TESTS_REGION fallback)
  --profile PROFILE    AWS CLI profile to use
  --expiry-days N      default: 7 — lifecycle expiry for objects under
                        kythira-real-test/
  --dry-run            print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bucket) BUCKET="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --expiry-days) EXPIRY_DAYS="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

aws_cli() {
    if [[ -n "${PROFILE}" ]]; then
        aws --profile "${PROFILE}" "$@"
    else
        aws "$@"
    fi
}

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'
        return 0
    fi
    "$@"
}

ACCOUNT_ID="$(aws_cli sts get-caller-identity --query Account --output text)"
CALLER_ARN="$(aws_cli sts get-caller-identity --query Arn --output text)"
: "${BUCKET:=kythira-ci-${ACCOUNT_ID}}"

echo "account:  ${ACCOUNT_ID}"
echo "caller:   ${CALLER_ARN}"
echo "bucket:   ${BUCKET}"
echo "region:   ${REGION}"

# The caller being the account root is workable for a one-off provisioning run
# and is NOT what CI should use. Say so rather than letting it pass silently.
if [[ "${CALLER_ARN}" == *":root" ]]; then
    echo "WARNING: running as the account root. Fine for provisioning; the CI" >&2
    echo "         role (kythira-ci-real-cloud-tests) is what the tests must" >&2
    echo "         use, scoped to this bucket and the ${TEST_PREFIX} prefix." >&2
fi

# ── 1. The bucket itself ─────────────────────────────────────────────────────
if aws_cli s3api head-bucket --bucket "${BUCKET}" >/dev/null 2>&1; then
    echo "bucket exists — leaving it alone"
else
    echo "creating bucket"
    # us-east-1 is the one region that rejects an explicit LocationConstraint.
    if [[ "${REGION}" == "us-east-1" ]]; then
        run aws_cli s3api create-bucket --bucket "${BUCKET}" --region "${REGION}"
    else
        run aws_cli s3api create-bucket --bucket "${BUCKET}" --region "${REGION}" \
            --create-bucket-configuration "LocationConstraint=${REGION}"
    fi
fi

# ── 2. Public access blocked, all four switches ──────────────────────────────
echo "blocking public access"
run aws_cli s3api put-public-access-block --bucket "${BUCKET}" \
    --public-access-block-configuration \
    "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true"

# ── 3. Default encryption ────────────────────────────────────────────────────
echo "enabling default encryption (SSE-S3)"
run aws_cli s3api put-bucket-encryption --bucket "${BUCKET}" \
    --server-side-encryption-configuration '{
        "Rules": [{
            "ApplyServerSideEncryptionByDefault": {"SSEAlgorithm": "AES256"},
            "BucketKeyEnabled": true
        }]
    }'

# ── 4. Lifecycle: the test prefix expires; nothing else is touched ───────────
#
# Scoped to the prefix the real suites write under, deliberately: a rule
# without a prefix filter would expire an operator's objects too, and this
# bucket may not stay single-purpose.
echo "setting lifecycle expiry (${EXPIRY_DAYS} days) on ${TEST_PREFIX}"
run aws_cli s3api put-bucket-lifecycle-configuration --bucket "${BUCKET}" \
    --lifecycle-configuration "{
        \"Rules\": [{
            \"ID\": \"expire-kythira-real-test-prefix\",
            \"Status\": \"Enabled\",
            \"Filter\": {\"Prefix\": \"${TEST_PREFIX}\"},
            \"Expiration\": {\"Days\": ${EXPIRY_DAYS}},
            \"AbortIncompleteMultipartUpload\": {\"DaysAfterInitiation\": 1}
        }]
    }"

echo
echo "done. Set the repository variable:"
echo "  AWS_OBJECT_PERSISTENCE_BUCKET=${BUCKET}"
echo "  AWS_REAL_CLOUD_TESTS_REGION=${REGION}   (only if not already the default)"
echo
echo "NOT done by this script, and still required before CI can use it:"
echo "  * the object-persistence bundle on the CI role's policy. Run:"
echo "      scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \\"
echo "          --github-org ORG --github-repo REPO \\"
echo "          --bundles <every bundle you want>,object-persistence \\"
echo "          --bucket ${BUCKET}"
echo "    That grants get/put/delete on"
echo "    arn:aws:s3:::${BUCKET}/${TEST_PREFIX}* and prefix-conditioned"
echo "    ListBucket on the bucket — no bucket admin. NOTE that"
echo "    put-role-policy REPLACES the inline policy, so name every bundle"
echo "    the role should keep, not just this one."
