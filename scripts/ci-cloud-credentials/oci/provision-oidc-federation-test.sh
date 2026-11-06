#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time OCI setup for the real-cloud OIDC federation run
# (.kiro/specs/iam-compatible-auth/ task 5, .github/workflows/oidc-federation.yml).
# Run once, locally, with an oci CLI profile that may administer the identity
# domain and tenancy policies. The CI group may do neither.
#
# The path the test takes is the identity domain's token exchange
# (oci_upst_source): the Kythira JWT plus a fresh session public key go to
# {domain}/oauth2/v1/token, authenticated as a confidential app, and come back
# as a UPST for a service user. This script reuses the confidential app CI
# already has for GitHub federation, kythira-ci-token-exchange (README.md,
# "Credentials"), whose id and secret are the real-cloud-tests environment
# secrets OCI_WIF_CLIENT_ID and OCI_WIF_CLIENT_SECRET. The pair authorises
# nothing alone: only a JWT matching a trust rule turns into a session.
#
# Creates (if absent):
#   1. A service user, kythira-oidc-fed (serviceUser: true; a regular user is
#      refused at exchange time), and a group of the same name holding it.
#   2. An identity propagation trust, kythira-oidc-federation-test, for the
#      Kythira issuer (the S3 issuer URL the AWS script printed): JWKS at
#      <issuer>/.well-known/jwks.json, the confidential app as its only
#      client, and one impersonation rule, sub eq workload:<subject-uri>,
#      mapping to the service user. The rule value is unquoted: quoted values
#      never match (README.md).
#   3. A private bucket holding only the test object,
#      test-object/federation.txt.
#   4. A policy letting the group read objects in that bucket only.
#
# The identity domain calls go through `oci raw-request` with raw SCIM
# bodies: the identity-domains CLI model silently drops URN-keyed attributes,
# which once produced a non-service user (README.md).
#
# Users, groups, trusts and policies are free; the bucket holds a few bytes.
# Safe to re-run.
#
# Usage:
#   scripts/ci-cloud-credentials/oci/provision-oidc-federation-test.sh \
#       --domain-url URL --client-id ID --compartment-id OCID \
#       --issuer-url URL [--region REGION] [--bucket NAME] \
#       [--subject-uri URI] [--profile NAME] [--delete] [--dry-run]
set -euo pipefail

DOMAIN_URL=""
CLIENT_ID=""
COMPARTMENT_ID=""
ISSUER_URL=""
REGION=""
BUCKET="kythira-oidc-fed"
SUBJECT_URI="spiffe://kythira.test/wl/oidc-federation"
PROFILE=""
USER_NAME="kythira-oidc-fed"
GROUP_NAME="kythira-oidc-fed"
TRUST_NAME="kythira-oidc-federation-test"
POLICY_NAME="kythira-oidc-federation-test"
TEST_KEY="test-object/federation.txt"
DELETE=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-oidc-federation-test.sh [OPTIONS]

One-time OCI setup for .github/workflows/oidc-federation.yml. Run with an
identity domain administrator's oci CLI profile. Safe to re-run.

Required:
  --domain-url URL      the identity domain, https://idcs-....identity.oraclecloud.com
                        (the OCI_DOMAIN_URL secret)
  --client-id ID        the token-exchange confidential app's client id
                        (the OCI_WIF_CLIENT_ID secret)
  --compartment-id OCID where the bucket and policy go
  --issuer-url URL      the issuer URL the AWS script printed,
                        https://<bucket>.s3.<region>.amazonaws.com/<prefix>

Optional:
  --region REGION       default: the profile's region
  --bucket NAME         default: kythira-oidc-fed
  --subject-uri URI     default: spiffe://kythira.test/wl/oidc-federation
  --profile NAME        oci CLI profile
  --delete              remove the policy, bucket, trust, group and user
  --dry-run             print the changing calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --domain-url) DOMAIN_URL="${2%/}"; shift 2 ;;
        --client-id) CLIENT_ID="$2"; shift 2 ;;
        --compartment-id) COMPARTMENT_ID="$2"; shift 2 ;;
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

if [[ -z "${DOMAIN_URL}" || -z "${COMPARTMENT_ID}" ||
      ( "${DELETE}" -eq 0 && ( -z "${CLIENT_ID}" || -z "${ISSUER_URL}" ) ) ]]; then
    usage >&2
    exit 2
fi

OCI=(oci)
[[ -n "${PROFILE}" ]] && OCI+=(--profile "${PROFILE}")
[[ -n "${REGION}" ]] && OCI+=(--region "${REGION}")

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        { printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'; } >&2
        return 0
    fi
    "$@"
}

# GET {domain}/admin/v1/<resource>?filter=<attr> eq "<value>"; prints the
# first match's id, or nothing.
scim_find() {
    local resource="$1" attr="$2" value="$3"
    local filter
    filter="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' \
        "${attr} eq \"${value}\"")"
    "${OCI[@]}" raw-request --http-method GET \
        --target-uri "${DOMAIN_URL}/admin/v1/${resource}?filter=${filter}" |
        python3 -c '
import json, sys
r = json.load(sys.stdin).get("data", {}).get("Resources", [])
print(r[0]["id"] if r else "")'
}

# POST a raw SCIM body; prints the new resource's id.
scim_create() {
    local resource="$1" body="$2"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        run "${OCI[@]}" raw-request --http-method POST \
            --target-uri "${DOMAIN_URL}/admin/v1/${resource}" --request-body "${body}"
        echo "dry-run-id"
        return 0
    fi
    "${OCI[@]}" raw-request --http-method POST \
        --target-uri "${DOMAIN_URL}/admin/v1/${resource}" --request-body "${body}" |
        python3 -c '
import json, sys
r = json.load(sys.stdin)
if not str(r.get("status", "")).startswith("201"):
    sys.exit("SCIM create failed: " + json.dumps(r.get("data")))
print(r["data"]["id"])'
}

scim_delete() {
    local resource="$1" id="$2"
    [[ -n "${id}" ]] || return 0
    run "${OCI[@]}" raw-request --http-method DELETE \
        --target-uri "${DOMAIN_URL}/admin/v1/${resource}/${id}?forceDelete=true" >/dev/null || true
}

NAMESPACE="$("${OCI[@]}" os ns get --query data --raw-output)"
TENANCY_ID="$("${OCI[@]}" iam compartment get --compartment-id "${COMPARTMENT_ID}" \
    --query 'data."compartment-id"' --raw-output 2>/dev/null || true)"
SUBJECT="workload:${SUBJECT_URI}"

echo "domain:    ${DOMAIN_URL}"
echo "issuer:    ${ISSUER_URL}"
echo "subject:   ${SUBJECT}"
echo "bucket:    ${NAMESPACE}/${BUCKET}"

USER_ID="$(scim_find Users userName "${USER_NAME}")"
GROUP_ID="$(scim_find Groups displayName "${GROUP_NAME}")"
TRUST_ID="$(scim_find IdentityPropagationTrusts name "${TRUST_NAME}")"

if [[ "${DELETE}" -eq 1 ]]; then
    POLICY_ID="$("${OCI[@]}" iam policy list --compartment-id "${COMPARTMENT_ID}" --all \
        --query "data[?name=='${POLICY_NAME}'].id | [0]" --raw-output 2>/dev/null || true)"
    [[ -n "${POLICY_ID}" && "${POLICY_ID}" != "null" ]] &&
        run "${OCI[@]}" iam policy delete --policy-id "${POLICY_ID}" --force || true
    run "${OCI[@]}" os object delete --bucket-name "${BUCKET}" --object-name "${TEST_KEY}" \
        --force || true
    run "${OCI[@]}" os bucket delete --bucket-name "${BUCKET}" --force || true
    scim_delete IdentityPropagationTrusts "${TRUST_ID}"
    scim_delete Groups "${GROUP_ID}"
    scim_delete Users "${USER_ID}"
    echo "done"
    exit 0
fi

# ── 1. Service user and group ────────────────────────────────────────────────
if [[ -n "${USER_ID}" ]]; then
    echo "service user exists (${USER_ID})"
else
    USER_ID="$(scim_create Users "$(python3 - "${USER_NAME}" <<'PY'
import json, sys
name = sys.argv[1]
print(json.dumps({
    "schemas": ["urn:ietf:params:scim:schemas:core:2.0:User",
                "urn:ietf:params:scim:schemas:oracle:idcs:extension:user:User"],
    "userName": name,
    "name": {"familyName": name},
    "description": "kythira-test=oidc-federation: impersonated by the Kythira issuer trust",
    "urn:ietf:params:scim:schemas:oracle:idcs:extension:user:User": {"serviceUser": True},
}))
PY
)")"
    echo "created service user ${USER_ID}"
fi

if [[ -n "${GROUP_ID}" ]]; then
    echo "group exists (${GROUP_ID}); making sure the user is a member"
    run "${OCI[@]}" raw-request --http-method PATCH \
        --target-uri "${DOMAIN_URL}/admin/v1/Groups/${GROUP_ID}" \
        --request-body "$(python3 - "${USER_ID}" <<'PY'
import json, sys
print(json.dumps({
    "schemas": ["urn:ietf:params:scim:api:messages:2.0:PatchOp"],
    "Operations": [{"op": "add", "path": "members",
                    "value": [{"value": sys.argv[1], "type": "User"}]}],
}))
PY
)" >/dev/null || true
else
    GROUP_ID="$(scim_create Groups "$(python3 - "${GROUP_NAME}" "${USER_ID}" <<'PY'
import json, sys
name, user = sys.argv[1:3]
print(json.dumps({
    "schemas": ["urn:ietf:params:scim:schemas:core:2.0:Group"],
    "displayName": name,
    "members": [{"value": user, "type": "User"}],
}))
PY
)")"
    echo "created group ${GROUP_ID}"
fi

# ── 2. Identity propagation trust ────────────────────────────────────────────
TRUST_BODY="$(python3 - "${TRUST_NAME}" "${ISSUER_URL}" "${CLIENT_ID}" "${SUBJECT}" "${USER_ID}" <<'PY'
import json, sys
name, issuer, client, subject, user = sys.argv[1:6]
print(json.dumps({
    "schemas": ["urn:ietf:params:scim:schemas:oracle:idcs:IdentityPropagationTrust"],
    "name": name,
    "description": "kythira-test=oidc-federation: the Kythira CA issuer",
    "type": "JWT",
    "issuer": issuer,
    "publicKeyEndpoint": issuer + "/.well-known/jwks.json",
    "active": True,
    "oauthClients": [client],
    "allowImpersonation": True,
    "subjectType": "User",
    "subjectClaimName": "sub",
    "subjectMappingAttribute": "userName",
    # Unquoted value: a quoted one never matches.
    "impersonationServiceUsers": [{"rule": "sub eq " + subject, "value": user}],
}))
PY
)"
if [[ -n "${TRUST_ID}" ]]; then
    echo "trust exists (${TRUST_ID}); replacing it with the current issuer and rule"
    run "${OCI[@]}" raw-request --http-method PUT \
        --target-uri "${DOMAIN_URL}/admin/v1/IdentityPropagationTrusts/${TRUST_ID}" \
        --request-body "${TRUST_BODY}" >/dev/null
else
    TRUST_ID="$(scim_create IdentityPropagationTrusts "${TRUST_BODY}")"
    echo "created trust ${TRUST_ID}"
fi

# ── 3. Bucket and test object ────────────────────────────────────────────────
if "${OCI[@]}" os bucket get --bucket-name "${BUCKET}" >/dev/null 2>&1; then
    echo "bucket exists"
else
    run "${OCI[@]}" os bucket create --compartment-id "${COMPARTMENT_ID}" --name "${BUCKET}" \
        --public-access-type NoPublicAccess \
        --freeform-tags '{"kythira-test":"oidc-federation"}' >/dev/null
fi
tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
printf 'kythira-oidc-federation-test\n' > "${tmp}"
run "${OCI[@]}" os object put --bucket-name "${BUCKET}" --name "${TEST_KEY}" --file "${tmp}" \
    --content-type text/plain --force >/dev/null

# ── 4. Read access to that bucket only ───────────────────────────────────────
# Groups of an identity domain are named '<domain display name>'/'<group>';
# the tenancy's own domain is 'Default'. A domain other than Default needs
# --profile pointing at it and this statement edited to match.
STATEMENT="Allow group 'Default'/'${GROUP_NAME}' to read objects in compartment id ${COMPARTMENT_ID} where target.bucket.name = '${BUCKET}'"
POLICY_ID="$("${OCI[@]}" iam policy list --compartment-id "${COMPARTMENT_ID}" --all \
    --query "data[?name=='${POLICY_NAME}'].id | [0]" --raw-output 2>/dev/null || true)"
STATEMENTS="$(python3 -c 'import json, sys; print(json.dumps([sys.argv[1]]))' "${STATEMENT}")"
if [[ -n "${POLICY_ID}" && "${POLICY_ID}" != "null" ]]; then
    echo "policy exists; replacing its statement"
    run "${OCI[@]}" iam policy update --policy-id "${POLICY_ID}" --statements "${STATEMENTS}" \
        --force >/dev/null
else
    run "${OCI[@]}" iam policy create --compartment-id "${COMPARTMENT_ID}" \
        --name "${POLICY_NAME}" --statements "${STATEMENTS}" \
        --description "kythira-test=oidc-federation: read the federation test object" \
        >/dev/null
fi

echo
echo "done (tenancy ${TENANCY_ID:-unknown}). Set these repository variables:"
echo "  OIDC_FED_OCI_REGION=${REGION:-<the region the bucket is in>}"
echo "  OIDC_FED_OCI_NAMESPACE=${NAMESPACE}"
echo "  OIDC_FED_OCI_BUCKET=${BUCKET}"
echo "The workflow reads the domain URL and the confidential app's id and secret"
echo "from the real-cloud-tests environment secrets OCI_DOMAIN_URL,"
echo "OCI_WIF_CLIENT_ID and OCI_WIF_CLIENT_SECRET, which already exist."
