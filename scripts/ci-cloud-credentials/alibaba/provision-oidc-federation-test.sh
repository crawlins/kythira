#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time Alibaba Cloud setup for the real-cloud OIDC federation run
# (.kiro/specs/iam-compatible-auth/ task 5, .github/workflows/oidc-federation.yml).
# Run once, locally, with an aliyun CLI profile that may administer RAM: the
# CI role never holds a RAM-write permission (provision-oidc-role.sh).
#
# Creates (if absent):
#   1. A RAM OIDC provider, kythira-ca, for the Kythira issuer (the S3 issuer
#      URL the AWS script printed), with client id sts.aliyuncs.com, the
#      audience alibaba_oidc_role_source requests. Alibaba requires TLS
#      fingerprints; every certificate in the issuer host's live chain is
#      registered (up to five), so a leaf rotation on the S3 endpoint does not
#      break the trust.
#   2. A RAM role, kythira-oidc-federation-test, whose trust policy pins
#      oidc:iss, oidc:aud and oidc:sub (workload:<subject-uri>) exactly.
#   3. A private OSS bucket holding only the test object,
#      test-object/federation.txt.
#   4. A custom policy letting the role read that one object, attached to it.
#
# RAM resources are free; the bucket holds a few bytes. Safe to re-run.
#
# Usage:
#   scripts/ci-cloud-credentials/alibaba/provision-oidc-federation-test.sh \
#       --issuer-url URL [--region REGION] [--bucket NAME] \
#       [--subject-uri URI] [--profile NAME] [--delete] [--dry-run]
set -euo pipefail

ISSUER_URL=""
REGION="cn-hangzhou"
BUCKET=""
SUBJECT_URI="spiffe://kythira.test/wl/oidc-federation"
PROFILE="${ALIYUN_PROFILE:-}"
PROVIDER_NAME="kythira-ca"
ROLE_NAME="kythira-oidc-federation-test"
POLICY_NAME="kythira-oidc-federation-test-read"
CLIENT_ID="sts.aliyuncs.com"
TEST_KEY="test-object/federation.txt"
DELETE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-oidc-federation-test.sh [OPTIONS]

One-time Alibaba Cloud setup for .github/workflows/oidc-federation.yml. Run
with a RAM administrator's aliyun CLI profile. Safe to re-run.

Required:
  --issuer-url URL      the issuer URL the AWS script printed,
                        https://<bucket>.s3.<region>.amazonaws.com/<prefix>

Optional:
  --region REGION       the OSS region; default: cn-hangzhou
  --bucket NAME         default: kythira-oidc-fed-<account id>
  --subject-uri URI     default: spiffe://kythira.test/wl/oidc-federation
  --profile NAME        aliyun CLI profile (or $ALIYUN_PROFILE)
  --delete              remove the policy, role, provider and bucket
  --dry-run             print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --issuer-url) ISSUER_URL="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --subject-uri) SUBJECT_URI="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --delete) DELETE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "${DELETE}" -eq 0 && -z "${ISSUER_URL}" ]]; then
    usage >&2
    exit 2
fi

ALI=(aliyun)
[[ -n "${PROFILE}" ]] && ALI=(aliyun --profile "${PROFILE}")

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        { printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'; } >&2
        return 0
    fi
    "$@"
}

command -v aliyun >/dev/null 2>&1 || { echo "ERROR: the aliyun CLI is not on PATH." >&2; exit 1; }
ACCOUNT_ID="$("${ALI[@]}" sts GetCallerIdentity |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["AccountId"])')"
: "${BUCKET:=kythira-oidc-fed-${ACCOUNT_ID}}"
PROVIDER_ARN="acs:ram::${ACCOUNT_ID}:oidc-provider/${PROVIDER_NAME}"
ROLE_ARN="acs:ram::${ACCOUNT_ID}:role/${ROLE_NAME}"
SUBJECT="workload:${SUBJECT_URI}"

echo "account:  ${ACCOUNT_ID}"
echo "issuer:   ${ISSUER_URL}"
echo "subject:  ${SUBJECT}"
echo "role:     ${ROLE_ARN}"
echo "bucket:   oss://${BUCKET} (${REGION})"

if [[ "${DELETE}" -eq 1 ]]; then
    run "${ALI[@]}" ram DetachPolicyFromRole --PolicyType Custom --PolicyName "${POLICY_NAME}" \
        --RoleName "${ROLE_NAME}" || true
    run "${ALI[@]}" ram DeletePolicy --PolicyName "${POLICY_NAME}" || true
    run "${ALI[@]}" ram DeleteRole --RoleName "${ROLE_NAME}" || true
    run "${ALI[@]}" ims DeleteOIDCProvider --OIDCProviderName "${PROVIDER_NAME}" || true
    run "${ALI[@]}" oss rm "oss://${BUCKET}" -r -f -b --region "${REGION}" || true
    echo "done"
    exit 0
fi

# ── 1. OIDC provider ─────────────────────────────────────────────────────────
# OIDC providers live under ims, not ram (see provision-oidc-role.sh).
#
# CreateOIDCProvider fetches and validates <issuer>/.well-known/
# openid-configuration, failing InvalidParameter.OIDCDiscovery when it is
# absent. Between federation runs it is absent on purpose: each run of
# oidc-federation.yml publishes the issuer documents and deletes them at the
# end. So when the provider does not exist yet, the documents have to be up
# for the create call: publish a placeholder discovery document and JWKS
# (any RSA key; token-time checks use the JWKS each run publishes), run this
# script, then delete them. That is how the provider was created on
# November 6, 2026.
ISSUER_HOST="${ISSUER_URL#https://}"
ISSUER_HOST="${ISSUER_HOST%%/*}"
CHAIN_DIR="$(mktemp -d)"
trap 'rm -rf "${CHAIN_DIR}"' EXIT
echo | openssl s_client -servername "${ISSUER_HOST}" -connect "${ISSUER_HOST}:443" -showcerts \
        2>/dev/null |
    awk -v d="${CHAIN_DIR}" '/BEGIN CERT/{n++} n>0{print > (d "/cert" n ".pem")}'
FINGERPRINTS=""
for cert in $(ls "${CHAIN_DIR}"/cert*.pem 2>/dev/null | sort -V | head -5); do
    fp="$(openssl x509 -in "${cert}" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g' |
          tr 'A-Z' 'a-z')"
    FINGERPRINTS="${FINGERPRINTS:+${FINGERPRINTS},}${fp}"
done
[[ -n "${FINGERPRINTS}" ]] || { echo "ERROR: could not read ${ISSUER_HOST}'s TLS chain." >&2; exit 1; }
echo "fingerprints: ${FINGERPRINTS}"

PROVIDER_EXISTS=0
"${ALI[@]}" ims GetOIDCProvider --OIDCProviderName "${PROVIDER_NAME}" >/dev/null 2>&1 &&
    PROVIDER_EXISTS=1
if [[ "${PROVIDER_EXISTS}" -eq 0 && "${DRY_RUN}" -eq 0 ]] &&
        ! curl -fsS -o /dev/null "${ISSUER_URL}/.well-known/openid-configuration"; then
    echo "ERROR: ${ISSUER_URL}/.well-known/openid-configuration is not reachable, and" >&2
    echo "       Alibaba reads it to create the provider. Publish a placeholder discovery" >&2
    echo "       document and JWKS there first (see the comment above), then re-run." >&2
    exit 1
fi
if [[ "${PROVIDER_EXISTS}" -eq 1 ]]; then
    # Adds any fingerprint the chain now has that the provider lacks, so a
    # re-run repairs a rotation. One already registered, or the five-print
    # cap, is an error to ignore; stale prints are left for an operator.
    echo "OIDC provider exists; adding any new fingerprints"
    for fp in ${FINGERPRINTS//,/ }; do
        run "${ALI[@]}" ims AddFingerprintToOIDCProvider --OIDCProviderName "${PROVIDER_NAME}" \
            --Fingerprint "${fp}" >/dev/null 2>&1 || true
    done
else
    run "${ALI[@]}" ims CreateOIDCProvider --OIDCProviderName "${PROVIDER_NAME}" \
        --IssuerUrl "${ISSUER_URL}" --ClientIds "${CLIENT_ID}" \
        --Fingerprints "${FINGERPRINTS}" \
        --Description "kythira-test=oidc-federation: the Kythira CA issuer" >/dev/null
fi

# ── 2. Role ──────────────────────────────────────────────────────────────────
# Alibaba policy Version is the string "1", not AWS's date.
TRUST="$(python3 - "${PROVIDER_ARN}" "${ISSUER_URL}" "${CLIENT_ID}" "${SUBJECT}" <<'PY'
import json, sys
arn, issuer, aud, subject = sys.argv[1:5]
print(json.dumps({"Version": "1", "Statement": [{
    "Effect": "Allow", "Action": "sts:AssumeRole",
    "Principal": {"Federated": [arn]},
    "Condition": {"StringEquals": {"oidc:iss": issuer, "oidc:aud": aud, "oidc:sub": subject}},
}]}))
PY
)"
if "${ALI[@]}" ram GetRole --RoleName "${ROLE_NAME}" >/dev/null 2>&1; then
    echo "role exists; updating its trust policy"
    run "${ALI[@]}" ram UpdateRole --RoleName "${ROLE_NAME}" \
        --NewAssumeRolePolicyDocument "${TRUST}" >/dev/null
else
    run "${ALI[@]}" ram CreateRole --RoleName "${ROLE_NAME}" \
        --AssumeRolePolicyDocument "${TRUST}" --MaxSessionDuration 3600 \
        --Description "kythira-test=oidc-federation (.kiro/specs/iam-compatible-auth/ task 5)" \
        >/dev/null
fi

# ── 3. Bucket and test object ────────────────────────────────────────────────
if "${ALI[@]}" oss stat "oss://${BUCKET}" --region "${REGION}" >/dev/null 2>&1; then
    echo "bucket exists"
else
    run "${ALI[@]}" oss mb "oss://${BUCKET}" --acl private --region "${REGION}" >/dev/null
fi
tmp="${CHAIN_DIR}/object.txt"
printf 'kythira-oidc-federation-test\n' > "${tmp}"
run "${ALI[@]}" oss cp "${tmp}" "oss://${BUCKET}/${TEST_KEY}" -f --region "${REGION}" >/dev/null

# ── 4. Read access to that object only ───────────────────────────────────────
POLICY="$(python3 - "${BUCKET}" "${TEST_KEY}" <<'PY'
import json, sys
bucket, key = sys.argv[1:3]
print(json.dumps({"Version": "1", "Statement": [{
    "Effect": "Allow", "Action": ["oss:GetObject"],
    "Resource": [f"acs:oss:*:*:{bucket}/{key}"],
}]}))
PY
)"
if "${ALI[@]}" ram GetPolicy --PolicyType Custom --PolicyName "${POLICY_NAME}" >/dev/null 2>&1; then
    echo "policy exists"
else
    run "${ALI[@]}" ram CreatePolicy --PolicyName "${POLICY_NAME}" \
        --PolicyDocument "${POLICY}" \
        --Description "kythira-test=oidc-federation: read the federation test object" >/dev/null
fi
if "${ALI[@]}" ram ListPoliciesForRole --RoleName "${ROLE_NAME}" 2>/dev/null |
        grep -q "\"${POLICY_NAME}\""; then
    echo "policy attached"
else
    run "${ALI[@]}" ram AttachPolicyToRole --PolicyType Custom --PolicyName "${POLICY_NAME}" \
        --RoleName "${ROLE_NAME}" >/dev/null
fi

echo
echo "done. Set these repository variables:"
echo "  OIDC_FED_ALIBABA_ROLE_ARN=${ROLE_ARN}"
echo "  OIDC_FED_ALIBABA_PROVIDER_ARN=${PROVIDER_ARN}"
echo "  OIDC_FED_ALIBABA_REGION=${REGION}"
echo "  OIDC_FED_ALIBABA_BUCKET=${BUCKET}"
