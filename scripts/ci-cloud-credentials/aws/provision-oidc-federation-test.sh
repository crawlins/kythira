#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time AWS setup for the real-cloud OIDC federation run
# (.kiro/specs/iam-compatible-auth/ task 5, .github/workflows/oidc-federation.yml).
# Run once, locally, with IAM admin credentials: the CI role's permissions
# boundary denies every IAM action, so CI cannot create these itself.
#
# Creates (if absent) and configures:
#   1. The issuer bucket. Only <prefix>/.well-known/* is publicly readable;
#      that is where ca_cluster_node --oidc-publish-url puts the discovery
#      document and JWKS, and where AWS and GCP fetch them. The CI role may
#      write and delete exactly there (bucket policy; its inline policy has
#      no room left). Everything else stays private.
#   2. The test object, test-object/federation.txt, private.
#   3. An IAM OIDC provider for the issuer URL
#      https://<bucket>.s3.<region>.amazonaws.com/<prefix>, client id
#      sts.amazonaws.com.
#   4. A role that only that provider can assume, only for a token whose aud
#      is sts.amazonaws.com and whose sub is workload:<subject-uri>, allowed
#      s3:GetObject on the test object and nothing else.
#
# All of it is free except the bucket's storage, a few bytes. Every resource
# carries the tag kythira-test=oidc-federation. Safe to re-run.
#
# The account-level S3 Block Public Access settings must allow a public
# bucket policy. The script checks and stops if they do not.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-oidc-federation-test.sh \
#       [--profile PROFILE] [--region REGION] [--bucket NAME] [--prefix P] \
#       [--subject-uri URI] [--role-name NAME] [--ci-role-name NAME] [--dry-run]
#
# Remove everything with --delete (needs the same admin credentials).
set -euo pipefail

PROFILE=""
REGION="us-east-1"
BUCKET=""
PREFIX="ci"
SUBJECT_URI="spiffe://kythira.test/wl/oidc-federation"
ROLE_NAME="kythira-oidc-federation-test-role"
CI_ROLE_NAME="kythira-ci-real-cloud-tests"
TEST_KEY="test-object/federation.txt"
TEST_BODY=$'kythira-oidc-federation-test\n'
DELETE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-oidc-federation-test.sh [OPTIONS]

One-time AWS setup for .github/workflows/oidc-federation.yml. Run with IAM
admin credentials. Safe to re-run.

Optional:
  --profile PROFILE     AWS CLI profile
  --region REGION       default: us-east-1
  --bucket NAME         default: kythira-oidc-fed-<account-id>
  --prefix P            issuer path under the bucket (default: ci)
  --subject-uri URI     workload URI the role trusts
                        (default: spiffe://kythira.test/wl/oidc-federation)
  --role-name NAME      default: kythira-oidc-federation-test-role
  --ci-role-name NAME   role that publishes the documents
                        (default: kythira-ci-real-cloud-tests)
  --delete              remove the role, the OIDC provider and the bucket
  --dry-run             print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile) PROFILE="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --subject-uri) SUBJECT_URI="$2"; shift 2 ;;
        --role-name) ROLE_NAME="$2"; shift 2 ;;
        --ci-role-name) CI_ROLE_NAME="$2"; shift 2 ;;
        --delete) DELETE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

aws_cli() {
    if [[ -n "${PROFILE}" ]]; then
        aws --profile "${PROFILE}" --region "${REGION}" "$@"
    else
        aws --region "${REGION}" "$@"
    fi
}

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        { printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'; } >&2
        return 0
    fi
    "$@"
}

PREFIX="${PREFIX#/}"
PREFIX="${PREFIX%/}"
ACCOUNT_ID="$(aws_cli sts get-caller-identity --query Account --output text)"
: "${BUCKET:=kythira-oidc-fed-${ACCOUNT_ID}}"
ISSUER_HOSTPATH="${BUCKET}.s3.${REGION}.amazonaws.com/${PREFIX}"
ISSUER_URL="https://${ISSUER_HOSTPATH}"
PROVIDER_ARN="arn:aws:iam::${ACCOUNT_ID}:oidc-provider/${ISSUER_HOSTPATH}"
CI_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${CI_ROLE_NAME}"
TAG_KEY="kythira-test"
TAG_VALUE="oidc-federation"

echo "account:  ${ACCOUNT_ID}"
echo "bucket:   ${BUCKET}"
echo "issuer:   ${ISSUER_URL}"
echo "role:     ${ROLE_NAME}"
echo "subject:  workload:${SUBJECT_URI}"

if [[ "${DELETE}" -eq 1 ]]; then
    echo "deleting the role, the OIDC provider and the bucket"
    run aws_cli iam delete-role-policy --role-name "${ROLE_NAME}" \
        --policy-name read-test-object || true
    run aws_cli iam delete-role --role-name "${ROLE_NAME}" || true
    run aws_cli iam delete-open-id-connect-provider \
        --open-id-connect-provider-arn "${PROVIDER_ARN}" || true
    run aws_cli s3 rb "s3://${BUCKET}" --force || true
    echo "done"
    exit 0
fi

# ── 0. Account-level Block Public Access must not forbid a public policy ─────
if pab="$(aws_cli s3control get-public-access-block --account-id "${ACCOUNT_ID}" \
            --query 'PublicAccessBlockConfiguration.[BlockPublicPolicy,RestrictPublicBuckets]' \
            --output text 2>/dev/null)"; then
    if [[ "${pab}" == *True* ]]; then
        echo "ERROR: the account's S3 Block Public Access blocks public bucket policies" >&2
        echo "       (BlockPublicPolicy/RestrictPublicBuckets: ${pab})." >&2
        echo "       The issuer documents must be publicly readable. Either relax those" >&2
        echo "       two account settings, or host the issuer elsewhere." >&2
        exit 1
    fi
fi

# ── 1. The issuer bucket ─────────────────────────────────────────────────────
if aws_cli s3api head-bucket --bucket "${BUCKET}" >/dev/null 2>&1; then
    echo "bucket exists"
else
    echo "creating bucket"
    if [[ "${REGION}" == "us-east-1" ]]; then
        run aws_cli s3api create-bucket --bucket "${BUCKET}"
    else
        run aws_cli s3api create-bucket --bucket "${BUCKET}" \
            --create-bucket-configuration "LocationConstraint=${REGION}"
    fi
fi
run aws_cli s3api put-bucket-tagging --bucket "${BUCKET}" \
    --tagging "TagSet=[{Key=${TAG_KEY},Value=${TAG_VALUE}}]"
# ACLs stay blocked; only the bucket policy below may grant public reads.
run aws_cli s3api put-public-access-block --bucket "${BUCKET}" \
    --public-access-block-configuration \
    "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=false,RestrictPublicBuckets=false"
run aws_cli s3api put-bucket-encryption --bucket "${BUCKET}" \
    --server-side-encryption-configuration \
    '{"Rules":[{"ApplyServerSideEncryptionByDefault":{"SSEAlgorithm":"AES256"},"BucketKeyEnabled":true}]}'

echo "bucket policy: public read of ${PREFIX}/.well-known/*, CI role writes there"
run aws_cli s3api put-bucket-policy --bucket "${BUCKET}" --policy "{
    \"Version\": \"2012-10-17\",
    \"Statement\": [{
        \"Sid\": \"PublicIssuerDocuments\",
        \"Effect\": \"Allow\",
        \"Principal\": \"*\",
        \"Action\": \"s3:GetObject\",
        \"Resource\": \"arn:aws:s3:::${BUCKET}/${PREFIX}/.well-known/*\"
    }, {
        \"Sid\": \"CiPublishesIssuerDocuments\",
        \"Effect\": \"Allow\",
        \"Principal\": {\"AWS\": \"${CI_ROLE_ARN}\"},
        \"Action\": [\"s3:PutObject\", \"s3:DeleteObject\", \"s3:GetObject\"],
        \"Resource\": \"arn:aws:s3:::${BUCKET}/${PREFIX}/.well-known/*\"
    }, {
        \"Sid\": \"CiListsIssuerPrefix\",
        \"Effect\": \"Allow\",
        \"Principal\": {\"AWS\": \"${CI_ROLE_ARN}\"},
        \"Action\": \"s3:ListBucket\",
        \"Resource\": \"arn:aws:s3:::${BUCKET}\",
        \"Condition\": {\"StringLike\": {\"s3:prefix\": [\"${PREFIX}/.well-known/*\"]}}
    }]
}"

# ── 2. The test object ───────────────────────────────────────────────────────
echo "writing ${TEST_KEY}"
tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
printf '%s' "${TEST_BODY}" > "${tmp}"
run aws_cli s3api put-object --bucket "${BUCKET}" --key "${TEST_KEY}" --body "${tmp}" \
    --content-type text/plain --tagging "${TAG_KEY}=${TAG_VALUE}" >/dev/null

# ── 3. The IAM OIDC provider ─────────────────────────────────────────────────
# IAM ignores thumbprints for an issuer served by S3 (it trusts the
# certificate through its own root store), but without --thumbprint-list it
# fetches the discovery document at creation time to compute one, and that
# fails here: nothing is published until a run. So pass Amazon Root CA 1's
# SHA-1, the top of the chain S3 serves (measured November 5, 2026).
AMAZON_ROOT_CA1_SHA1="06b25927c42a721631c1efd9431e648fa62e1e39"
if aws_cli iam get-open-id-connect-provider \
        --open-id-connect-provider-arn "${PROVIDER_ARN}" >/dev/null 2>&1; then
    echo "OIDC provider exists"
else
    echo "creating OIDC provider"
    run aws_cli iam create-open-id-connect-provider --url "${ISSUER_URL}" \
        --client-id-list sts.amazonaws.com \
        --thumbprint-list "${AMAZON_ROOT_CA1_SHA1}" \
        --tags "Key=${TAG_KEY},Value=${TAG_VALUE}" >/dev/null
fi

# ── 4. The role ──────────────────────────────────────────────────────────────
TRUST="{
    \"Version\": \"2012-10-17\",
    \"Statement\": [{
        \"Effect\": \"Allow\",
        \"Principal\": {\"Federated\": \"${PROVIDER_ARN}\"},
        \"Action\": \"sts:AssumeRoleWithWebIdentity\",
        \"Condition\": {\"StringEquals\": {
            \"${ISSUER_HOSTPATH}:aud\": \"sts.amazonaws.com\",
            \"${ISSUER_HOSTPATH}:sub\": \"workload:${SUBJECT_URI}\"
        }}
    }]
}"
if aws_cli iam get-role --role-name "${ROLE_NAME}" >/dev/null 2>&1; then
    echo "role exists; updating its trust policy"
    run aws_cli iam update-assume-role-policy --role-name "${ROLE_NAME}" \
        --policy-document "${TRUST}"
else
    echo "creating role"
    run aws_cli iam create-role --role-name "${ROLE_NAME}" \
        --assume-role-policy-document "${TRUST}" --max-session-duration 3600 \
        --description "Kythira OIDC federation test: read one test object" \
        --tags "Key=${TAG_KEY},Value=${TAG_VALUE}" >/dev/null
fi
run aws_cli iam put-role-policy --role-name "${ROLE_NAME}" --policy-name read-test-object \
    --policy-document "{
        \"Version\": \"2012-10-17\",
        \"Statement\": [{
            \"Effect\": \"Allow\",
            \"Action\": \"s3:GetObject\",
            \"Resource\": \"arn:aws:s3:::${BUCKET}/${TEST_KEY}\"
        }]
    }"

echo
echo "done. Set these repository variables:"
echo "  OIDC_FED_AWS_BUCKET=${BUCKET}"
echo "  OIDC_FED_AWS_PREFIX=${PREFIX}"
echo "  OIDC_FED_AWS_REGION=${REGION}"
echo "  OIDC_FED_AWS_ROLE_ARN=arn:aws:iam::${ACCOUNT_ID}:role/${ROLE_NAME}"
echo "and give the GCP script this issuer URL: ${ISSUER_URL}"
