#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# provision-oidc-role.sh — create the RAM identity GitHub Actions assumes for
# Alibaba Cloud real-cloud tests, scoped to exactly the bundles given.
#
# Mirrors scripts/ci-cloud-credentials/aws/provision-oidc-role.sh in shape and
# in safety properties: idempotent, --dry-run-able, and the CI role it creates
# never holds a RAM-write permission, so a compromised test run cannot expand
# its own footprint.
#
# Run by a human operator with admin-ish local credentials (an `aliyun` CLI
# profile), never by CI itself. CI receives only short-lived STS credentials
# federated from its GitHub OIDC token — no long-lived key is stored anywhere.
#
# Re-running with a different --bundles set REPLACES the role's policy content
# (CreatePolicyVersion --SetAsDefault), so removing a bundle genuinely revokes
# its permissions rather than merely ceasing to use them.

set -euo pipefail

GITHUB_ORG=""
GITHUB_REPO=""
BUNDLES=""
ROLE_NAME="kythira-ci-real-cloud-tests"
OIDC_PROVIDER_NAME="github-actions"
POLICY_NAME_PREFIX="kythira-ci"
BUCKET=""
REF_RESTRICTION=""
PROFILE="${ALIYUN_PROFILE:-}"
DRY_RUN=0

ISSUER_URL="https://token.actions.githubusercontent.com"
# The audience the official aliyun/configure-aliyun-credentials-action requests.
CLIENT_ID="sts.aliyuncs.com"
# Computed from the issuer's live TLS chain when left empty (see below), so a
# CA rotation cannot leave a stale constant in the tree.
FINGERPRINT=""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POLICY_DIR="${SCRIPT_DIR}/policies"

usage() {
    cat >&2 <<'USAGE'
Usage: provision-oidc-role.sh --github-org ORG --github-repo REPO \
           --bundles BUNDLE[,BUNDLE...] [--bucket NAME] \
           [--role-name NAME] [--oidc-provider-name NAME] \
           [--ref-restriction SUB] [--profile ALIYUN_PROFILE] [--dry-run]

  --bundles            ess-quorum-manager,oss-persistence (any non-empty subset)
  --bucket             OSS bucket name; required when the oss-persistence
                       bundle is selected (substituted for {{BUCKET}})
  --ref-restriction    further restricts the trusted OIDC subject, e.g.
                       "repo:ORG/REPO:environment:real-cloud-tests".
                       Default: repo:ORG/REPO:*
  --profile            aliyun CLI profile to use (or $ALIYUN_PROFILE)
  --dry-run            print the calls that would run without executing them
USAGE
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --github-org) GITHUB_ORG="$2"; shift 2 ;;
        --github-repo) GITHUB_REPO="$2"; shift 2 ;;
        --bundles) BUNDLES="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --role-name) ROLE_NAME="$2"; shift 2 ;;
        --oidc-provider-name) OIDC_PROVIDER_NAME="$2"; shift 2 ;;
        --ref-restriction) REF_RESTRICTION="$2"; shift 2 ;;
        --fingerprint) FINGERPRINT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage ;;
        *) echo "ERROR: unknown argument: $1" >&2; usage ;;
    esac
done

[[ -z "${GITHUB_ORG}" || -z "${GITHUB_REPO}" || -z "${BUNDLES}" ]] && {
    echo "ERROR: --github-org, --github-repo and --bundles are all required." >&2
    usage
}

ALI=(aliyun)
[[ -n "${PROFILE}" ]] && ALI=(aliyun --profile "${PROFILE}")

run() {
    if [[ "${DRY_RUN}" == "1" ]]; then
        printf '[dry-run]'
        printf ' %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

command -v aliyun >/dev/null 2>&1 || {
    echo "ERROR: the 'aliyun' CLI is not on PATH." >&2
    exit 1
}

echo "[step] Verify local credentials"
CALLER="$("${ALI[@]}" sts GetCallerIdentity 2>/dev/null)" || {
    echo "ERROR: 'aliyun sts GetCallerIdentity' failed — configure a profile first." >&2
    exit 1
}
ACCOUNT_ID="$(printf '%s' "${CALLER}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["AccountId"])')"
CALLER_ARN="$(printf '%s' "${CALLER}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["Arn"])')"
echo "  account: ${ACCOUNT_ID}"
echo "  caller:  ${CALLER_ARN}"

# ── OIDC provider ────────────────────────────────────────────────────────────
# One per issuer per account; shared by every role that federates from GitHub.
# NOTE the product: OIDC identity-provider operations live under **ims**
# (Identity Management Service), NOT ram. `aliyun ram CreateOIDCProvider`
# fails "not a valid api", and forcing it against ram.aliyuncs.com/2015-05-01
# is rejected by the server itself with InvalidAction.NotFound — a genuine
# product split, not stale CLI metadata.
#
# NOTE also that Fingerprints is MANDATORY here, unlike AWS's equivalent where
# the thumbprint is optional and auto-derived: omitting it fails
# MissingFingerprints.
echo "[step] Ensure OIDC provider: ${OIDC_PROVIDER_NAME}"
if "${ALI[@]}" ims GetOIDCProvider --OIDCProviderName "${OIDC_PROVIDER_NAME}" >/dev/null 2>&1; then
    echo "  already exists"
else
    if [[ -z "${FINGERPRINT}" ]]; then
        echo "  computing the issuer root-CA thumbprint from its live TLS chain"
        ISSUER_HOST="${ISSUER_URL#https://}"
        CHAIN_DIR="$(mktemp -d)"
        echo | openssl s_client -servername "${ISSUER_HOST}" -connect "${ISSUER_HOST}:443" \
            -showcerts 2>/dev/null \
            | awk -v d="${CHAIN_DIR}" '/BEGIN CERT/{n++} n>0{print > (d "/cert" n ".pem")}'
        LAST_CERT="$(ls "${CHAIN_DIR}"/cert*.pem 2>/dev/null | sort -V | tail -1)"
        [[ -n "${LAST_CERT}" ]] || { echo "ERROR: could not fetch the issuer TLS chain." >&2; exit 1; }
        FINGERPRINT="$(openssl x509 -in "${LAST_CERT}" -noout -fingerprint -sha1 \
            | sed 's/.*=//; s/://g' | tr 'A-Z' 'a-z')"
        rm -rf "${CHAIN_DIR}"
        echo "  thumbprint: ${FINGERPRINT}"
    fi
    run "${ALI[@]}" ims CreateOIDCProvider \
        --OIDCProviderName "${OIDC_PROVIDER_NAME}" \
        --IssuerUrl "${ISSUER_URL}" \
        --ClientIds "${CLIENT_ID}" \
        --Fingerprints "${FINGERPRINT}"
fi

OIDC_PROVIDER_ARN="acs:ram::${ACCOUNT_ID}:oidc-provider/${OIDC_PROVIDER_NAME}"
ROLE_ARN="acs:ram::${ACCOUNT_ID}:role/${ROLE_NAME}"

# ── Trust policy ─────────────────────────────────────────────────────────────
# oidc:iss and oidc:aud are pinned exactly; the subject is pinned exactly when
# --ref-restriction is given, else matched with a prefix wildcard. Note the
# policy Version is the string "1" — Alibaba's own versioning, NOT AWS's
# "2012-10-17" date, and a wrong value here is rejected at create time.
SUBJECT="repo:${GITHUB_ORG}/${GITHUB_REPO}:*"
SUBJECT_OP="StringLike"
if [[ -n "${REF_RESTRICTION}" ]]; then
    SUBJECT="${REF_RESTRICTION}"
    SUBJECT_OP="StringEquals"
fi

TRUST_POLICY="$(python3 - "$OIDC_PROVIDER_ARN" "$ISSUER_URL" "$CLIENT_ID" "$SUBJECT" "$SUBJECT_OP" <<'PY'
import json, sys
arn, issuer, aud, subject, op = sys.argv[1:6]
cond = {"StringEquals": {"oidc:iss": issuer, "oidc:aud": aud}}
cond.setdefault(op, {})["oidc:sub"] = subject
print(json.dumps({
    "Version": "1",
    "Statement": [{
        "Effect": "Allow",
        "Action": "sts:AssumeRole",
        "Principal": {"Federated": [arn]},
        "Condition": cond,
    }],
}))
PY
)"

echo "[step] Ensure role: ${ROLE_NAME}"
if "${ALI[@]}" ram GetRole --RoleName "${ROLE_NAME}" >/dev/null 2>&1; then
    echo "  already exists — updating its trust policy"
    run "${ALI[@]}" ram UpdateRole --RoleName "${ROLE_NAME}" \
        --NewAssumeRolePolicyDocument "${TRUST_POLICY}"
else
    run "${ALI[@]}" ram CreateRole --RoleName "${ROLE_NAME}" \
        --AssumeRolePolicyDocument "${TRUST_POLICY}" \
        --Description "kythira CI real-cloud-tests OIDC role (.kiro/specs/alibaba-cloud-services/)"
fi

# ── Per-bundle permission policies ───────────────────────────────────────────
# One custom policy per bundle rather than one merged policy: attaching and
# detaching then maps 1:1 onto enabling and disabling a bundle, and a bundle's
# permissions are readable in isolation in the console.
IFS=',' read -ra BUNDLE_LIST <<< "${BUNDLES}"
for bundle in "${BUNDLE_LIST[@]}"; do
    POLICY_FILE="${POLICY_DIR}/${bundle}.json"
    [[ -f "${POLICY_FILE}" ]] || {
        echo "ERROR: unknown bundle '${bundle}' — no ${POLICY_FILE}" >&2
        echo "       valid bundles: $(ls "${POLICY_DIR}" | sed 's/\.json$//' | tr '\n' ' ')" >&2
        exit 1
    }

    if grep -q '{{BUCKET}}' "${POLICY_FILE}" && [[ -z "${BUCKET}" ]]; then
        echo "ERROR: bundle '${bundle}' needs --bucket (its policy scopes to {{BUCKET}})." >&2
        exit 1
    fi

    POLICY_NAME="${POLICY_NAME_PREFIX}-${bundle}"
    POLICY_DOC="$(python3 - "${POLICY_FILE}" "${BUCKET}" <<'PY'
import json, sys
path, bucket = sys.argv[1], sys.argv[2]
raw = open(path).read().replace("{{BUCKET}}", bucket)
print(json.dumps({"Version": "1", "Statement": json.loads(raw)}))
PY
)"

    echo "[step] Ensure policy: ${POLICY_NAME}"
    if "${ALI[@]}" ram GetPolicy --PolicyType Custom --PolicyName "${POLICY_NAME}" >/dev/null 2>&1; then
        # Alibaba caps a custom policy at 5 versions, so a script meant to be
        # re-run must prune or it eventually fails hard on LimitExceeded. Drop
        # the oldest non-default version when at the cap — the same constraint
        # that led the AWS spec to prefer inline policies over managed ones.
        VERSIONS="$("${ALI[@]}" ram ListPolicyVersions --PolicyType Custom \
            --PolicyName "${POLICY_NAME}" 2>/dev/null || true)"
        STALE="$(printf '%s' "${VERSIONS}" | python3 -c '
import json, sys
try:
    v = json.load(sys.stdin)["PolicyVersions"]["PolicyVersion"]
except Exception:
    sys.exit(0)
if len(v) >= 5:
    old = sorted((x for x in v if not x.get("IsDefaultVersion")),
                 key=lambda x: x.get("CreateDate", ""))
    if old:
        print(old[0]["VersionId"])
' 2>/dev/null || true)"
        if [[ -n "${STALE}" ]]; then
            echo "  pruning stale policy version ${STALE} (5-version cap)"
            run "${ALI[@]}" ram DeletePolicyVersion --PolicyName "${POLICY_NAME}" \
                --VersionId "${STALE}" >/dev/null
        fi
        # A new default version replaces the content wholesale, which is what
        # makes re-running with changed bundle contents actually revoke.
        run "${ALI[@]}" ram CreatePolicyVersion --PolicyName "${POLICY_NAME}" \
            --PolicyDocument "${POLICY_DOC}" --SetAsDefault true >/dev/null
    else
        run "${ALI[@]}" ram CreatePolicy --PolicyName "${POLICY_NAME}" \
            --PolicyDocument "${POLICY_DOC}" \
            --Description "kythira CI bundle: ${bundle}"
    fi

    # Checked rather than attempted-and-ignored: AttachPolicyToRole DOES error
    # when the policy is already attached, so a bare `|| true` would print an
    # alarming SDK.ServerError on every re-run of an idempotent script.
    if "${ALI[@]}" ram ListPoliciesForRole --RoleName "${ROLE_NAME}" 2>/dev/null \
        | grep -q "\"PolicyName\": \"${POLICY_NAME}\""; then
        echo "[step] ${POLICY_NAME} already attached to ${ROLE_NAME}"
    else
        echo "[step] Attach ${POLICY_NAME} to ${ROLE_NAME}"
        run "${ALI[@]}" ram AttachPolicyToRole --PolicyType Custom \
            --PolicyName "${POLICY_NAME}" --RoleName "${ROLE_NAME}" >/dev/null
    fi
done

echo
echo "Done."
echo "  role ARN:          ${ROLE_ARN}"
echo "  OIDC provider ARN: ${OIDC_PROVIDER_ARN}"
echo
echo "Set these repository variables next (this script only touches Alibaba Cloud):"
echo "  gh variable set ALIBABA_CI_ROLE_ARN --body '${ROLE_ARN}'"
echo "  gh variable set ALIBABA_CI_OIDC_PROVIDER_ARN --body '${OIDC_PROVIDER_ARN}'"
echo "  gh variable set ALIBABA_CI_REGION --body '<region, e.g. ap-southeast-1>'"
[[ -n "${BUCKET}" ]] && echo "  gh variable set ALIBABA_OSS_BUCKET --body '${BUCKET}'"
echo "  gh variable set REAL_CLOUD_TESTS_ALIBABA_ENABLED --body true   # when ready"
