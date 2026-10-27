#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

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
#       [--expiry-days N] [--encryption sse-s3|sse-kms] [--dry-run]
#
# --encryption sse-kms makes the encrypted-bucket variant
# (.kiro/specs/object-store-encrypted-bucket-checksums/): SSE-KMS with the
# AWS-managed aws/s3 key, Bucket Key off so every PUT really calls KMS. Its
# default name is kythira-objpersist-kms-<account-id>, and it becomes the
# repository variable AWS_OBJECT_PERSISTENCE_KMS_BUCKET. The CI role reaches it
# through a bucket policy this script applies, not through the role's inline
# policy: that policy is shared by every bundle and has no room left under
# IAM's 10,240-character limit (render-ci-policy.py --check).
set -euo pipefail

BUCKET=""
REGION="us-east-1"
PROFILE=""
EXPIRY_DAYS="7"
TEST_PREFIX="kythira-real-test/"
ENCRYPTION="sse-s3"
CI_ROLE_NAME="kythira-ci-real-cloud-tests"
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
  --encryption MODE    sse-s3 (default) or sse-kms (aws/s3 key, Bucket Key
                        off; default bucket kythira-objpersist-kms-<account>)
  --ci-role-name NAME  sse-kms only: role the bucket policy grants
                        (default: kythira-ci-real-cloud-tests)
  --dry-run            print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bucket) BUCKET="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --expiry-days) EXPIRY_DAYS="$2"; shift 2 ;;
        --encryption) ENCRYPTION="$2"; shift 2 ;;
        --ci-role-name) CI_ROLE_NAME="$2"; shift 2 ;;
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

case "${ENCRYPTION}" in
    sse-s3|sse-kms) ;;
    *) echo "unknown --encryption: ${ENCRYPTION} (sse-s3 or sse-kms)" >&2; exit 2 ;;
esac

ACCOUNT_ID="$(aws_cli sts get-caller-identity --query Account --output text)"
CALLER_ARN="$(aws_cli sts get-caller-identity --query Arn --output text)"
if [[ "${ENCRYPTION}" == "sse-kms" ]]; then
    : "${BUCKET:=kythira-objpersist-kms-${ACCOUNT_ID}}"
else
    : "${BUCKET:=kythira-ci-${ACCOUNT_ID}}"
fi

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
# SSE-KMS keeps Bucket Key off on purpose: the point of that bucket is that
# every PUT goes through KMS, whose ETag is then not the content MD5. The
# AWS-managed aws/s3 key has no monthly charge, and its key policy already
# admits any principal in the account calling through S3, so the CI role needs
# no KMS grant of its own.
if [[ "${ENCRYPTION}" == "sse-kms" ]]; then
    echo "enabling default encryption (SSE-KMS, aws/s3 key, Bucket Key off)"
    run aws_cli s3api put-bucket-encryption --bucket "${BUCKET}" \
        --server-side-encryption-configuration '{
            "Rules": [{
                "ApplyServerSideEncryptionByDefault": {
                    "SSEAlgorithm": "aws:kms",
                    "KMSMasterKeyID": "alias/aws/s3"
                },
                "BucketKeyEnabled": false
            }]
        }'
else
    echo "enabling default encryption (SSE-S3)"
    run aws_cli s3api put-bucket-encryption --bucket "${BUCKET}" \
        --server-side-encryption-configuration '{
            "Rules": [{
                "ApplyServerSideEncryptionByDefault": {"SSEAlgorithm": "AES256"},
                "BucketKeyEnabled": true
            }]
        }'
fi

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

# ── 5. SSE-KMS only: the CI role's access, as a bucket policy ────────────────
#
# The same grant the object-persistence bundle gives the SSE-S3 bucket:
# object get/put/delete under the test prefix, and a listing limited to that
# prefix. A same-account bucket policy naming the role is enough on its own;
# the role's permissions boundary allows S3. KMS needs nothing here, because
# the aws/s3 key's policy admits any principal in the account through S3.
if [[ "${ENCRYPTION}" == "sse-kms" ]]; then
    CI_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${CI_ROLE_NAME}"
    echo "granting ${CI_ROLE_ARN} the test prefix via the bucket policy"
    run aws_cli s3api put-bucket-policy --bucket "${BUCKET}" --policy "{
        \"Version\": \"2012-10-17\",
        \"Statement\": [{
            \"Sid\": \"CiObjectPlane\",
            \"Effect\": \"Allow\",
            \"Principal\": {\"AWS\": \"${CI_ROLE_ARN}\"},
            \"Action\": [\"s3:GetObject\", \"s3:PutObject\", \"s3:DeleteObject\"],
            \"Resource\": \"arn:aws:s3:::${BUCKET}/${TEST_PREFIX}*\"
        }, {
            \"Sid\": \"CiListTestPrefixOnly\",
            \"Effect\": \"Allow\",
            \"Principal\": {\"AWS\": \"${CI_ROLE_ARN}\"},
            \"Action\": \"s3:ListBucket\",
            \"Resource\": \"arn:aws:s3:::${BUCKET}\",
            \"Condition\": {\"StringLike\": {\"s3:prefix\": [\"${TEST_PREFIX}\", \"${TEST_PREFIX}*\"]}}
        }]
    }"
fi

echo
echo "done. Set the repository variable:"
if [[ "${ENCRYPTION}" == "sse-kms" ]]; then
    echo "  AWS_OBJECT_PERSISTENCE_KMS_BUCKET=${BUCKET}"
else
    echo "  AWS_OBJECT_PERSISTENCE_BUCKET=${BUCKET}"
fi
echo "  AWS_REAL_CLOUD_TESTS_REGION=${REGION}   (only if not already the default)"
echo
if [[ "${ENCRYPTION}" == "sse-kms" ]]; then
    exit 0
fi
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
