#!/usr/bin/env bash
# Creates everything GitHub Actions CI needs to authenticate to AWS via
# OIDC federation — no long-lived access key ever generated or stored as
# a GitHub secret. Creates (if absent) the GitHub Actions OIDC identity
# provider, an IAM role trusted only by that provider for this exact
# repository, and a least-privilege inline policy scoped to exactly the
# --bundles given.
#
# Separate from, and does not create, provision-quorum-test-node-role.sh's
# static node role — that script must be run first if --bundles includes
# ec2-quorum-manager, since this script's policy references its role ARN.
#
# .kiro/specs/ci-real-cloud-tests/ Requirement 4 / design.md Component 2.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-oidc-role.sh \
#       --github-org ORG --github-repo REPO --bundles BUNDLE[,BUNDLE...] \
#       [--role-name NAME] [--session-duration-seconds N] \
#       [--ref-restriction REF] [--dry-run]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
POLICY_DIR="${REPO_ROOT}/scripts/ci-cloud-credentials/aws/policies"

GITHUB_ORG=""
GITHUB_REPO=""
BUNDLES=""
ROLE_NAME="kythira-ci-real-cloud-tests"
SESSION_DURATION="3600"
REF_RESTRICTION=""
DRY_RUN=0

OIDC_PROVIDER_URL="token.actions.githubusercontent.com"
OIDC_AUDIENCE="sts.amazonaws.com"
NODE_ROLE_NAME_DEFAULT="kythira-aws-quorum-test-node-role"

usage() {
    cat <<'EOF'
Usage: provision-oidc-role.sh [OPTIONS]

Creates the IAM role GitHub Actions CI assumes via OIDC federation, with
permissions scoped to exactly the bundles given. Safe to re-run — running
again with a different --bundles value replaces the role's policy content
(disabling a bundle and re-running revokes that bundle's permissions).

Required:
  --github-org ORG        e.g. crawlins
  --github-repo REPO      e.g. kythira
  --bundles LIST          Comma-separated: ec2-quorum-manager,ca-cluster-node,
                           ca-cluster-node-rpc-tls (any non-empty subset)

Optional:
  --role-name NAME               default: kythira-ci-real-cloud-tests
  --session-duration-seconds N   default: 3600 (one hour)
  --ref-restriction REF          e.g. "ref:refs/heads/main" — further
                                  restricts the trust policy's subject
                                  condition beyond repo:ORG/REPO:*
  --dry-run                      Print the AWS CLI calls that would run
                                  without executing them
  -h, --help                     Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --github-org) GITHUB_ORG="$2"; shift 2 ;;
        --github-repo) GITHUB_REPO="$2"; shift 2 ;;
        --bundles) BUNDLES="$2"; shift 2 ;;
        --role-name) ROLE_NAME="$2"; shift 2 ;;
        --session-duration-seconds) SESSION_DURATION="$2"; shift 2 ;;
        --ref-restriction) REF_RESTRICTION="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${GITHUB_ORG}" || -z "${GITHUB_REPO}" || -z "${BUNDLES}" ]]; then
    echo "ERROR: --github-org, --github-repo, and --bundles are all required." >&2
    usage >&2
    exit 1
fi

run() {
    if [[ "${DRY_RUN}" == "1" ]]; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

echo "[step] Sanity check: local AWS credentials"
if ! ACCOUNT_ID=$(aws sts get-caller-identity --query 'Account' --output text 2>/dev/null); then
    echo "ERROR: 'aws sts get-caller-identity' failed — configure local AWS credentials" \
         "with IAM admin rights before running this script." >&2
    exit 1
fi
echo "  account: ${ACCOUNT_ID}"

echo "[step] Ensure GitHub Actions OIDC identity provider"
EXISTING_PROVIDER_ARN=$(aws iam list-open-id-connect-providers \
    --query "OpenIDConnectProviderList[?contains(Arn, '${OIDC_PROVIDER_URL}')].Arn | [0]" \
    --output text 2>/dev/null || true)
if [[ -n "${EXISTING_PROVIDER_ARN}" && "${EXISTING_PROVIDER_ARN}" != "None" ]]; then
    echo "  already exists: ${EXISTING_PROVIDER_ARN}"
else
    # AWS documents this thumbprint requirement for GitHub's OIDC provider;
    # aws iam create-open-id-connect-provider requires at least one entry
    # even though AWS no longer validates it against the actual TLS chain
    # for well-known providers — see AWS's own GitHub OIDC setup guide.
    run aws iam create-open-id-connect-provider \
        --url "https://${OIDC_PROVIDER_URL}" \
        --client-id-list "${OIDC_AUDIENCE}" \
        --thumbprint-list "6938fd4d98bab03faadb97b34396831e3780aea"
fi

echo "[step] Build trust policy"
SUBJECT="repo:${GITHUB_ORG}/${GITHUB_REPO}:*"
if [[ -n "${REF_RESTRICTION}" ]]; then
    SUBJECT="repo:${GITHUB_ORG}/${GITHUB_REPO}:${REF_RESTRICTION}"
fi
TRUST_POLICY=$(cat <<EOF
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Principal": {"Federated": "arn:aws:iam::${ACCOUNT_ID}:oidc-provider/${OIDC_PROVIDER_URL}"},
            "Action": "sts:AssumeRoleWithWebIdentity",
            "Condition": {
                "StringEquals": {"${OIDC_PROVIDER_URL}:aud": "${OIDC_AUDIENCE}"},
                "StringLike": {"${OIDC_PROVIDER_URL}:sub": "${SUBJECT}"}
            }
        }
    ]
}
EOF
)

echo "[step] Ensure IAM role: ${ROLE_NAME}"
if aws iam get-role --role-name "${ROLE_NAME}" >/dev/null 2>&1; then
    echo "  already exists"
    run aws iam update-assume-role-policy \
        --role-name "${ROLE_NAME}" \
        --policy-document "${TRUST_POLICY}"
else
    run aws iam create-role \
        --role-name "${ROLE_NAME}" \
        --assume-role-policy-document "${TRUST_POLICY}" \
        --max-session-duration "${SESSION_DURATION}" \
        --description "kythira CI real-cloud-tests OIDC role — .kiro/specs/ci-real-cloud-tests/"
fi
# No permissions boundary is attached — this role never holds
# iam:CreateRole (or any IAM-write action), so there is nothing for a
# boundary to constrain. .kiro/specs/ci-real-cloud-tests/ Requirement 3.3.

echo "[step] Build bundle policy for: ${BUNDLES}"
STATEMENTS="[]"
IFS=',' read -ra BUNDLE_LIST <<< "${BUNDLES}"
for bundle in "${BUNDLE_LIST[@]}"; do
    POLICY_FILE="${POLICY_DIR}/${bundle}.json"
    if [[ ! -f "${POLICY_FILE}" ]]; then
        echo "ERROR: unknown bundle '${bundle}' — no ${POLICY_FILE}" >&2
        echo "       valid bundles: $(ls "${POLICY_DIR}" | sed 's/\.json$//' | tr '\n' ' ')" >&2
        exit 1
    fi
    BUNDLE_STATEMENTS=$(sed "s/{{ACCOUNT_ID}}/${ACCOUNT_ID}/g" "${POLICY_FILE}")
    STATEMENTS=$(python3 -c "
import json, sys
a = json.loads(sys.argv[1])
b = json.loads(sys.argv[2])
print(json.dumps(a + b))
" "${STATEMENTS}" "${BUNDLE_STATEMENTS}")
done
BUNDLE_POLICY=$(python3 -c "
import json, sys
print(json.dumps({'Version': '2012-10-17', 'Statement': json.loads(sys.argv[1]) + [
    {'Sid': 'StsGetCallerIdentity', 'Effect': 'Allow', 'Action': 'sts:GetCallerIdentity', 'Resource': '*'}
]}))
" "${STATEMENTS}")

echo "[step] Ensure inline policy: ${ROLE_NAME}-policy"
# put-role-policy replaces the named policy's content wholesale — running
# this script again with a bundle removed from --bundles genuinely revokes
# that bundle's permissions, not merely stops using them.
run aws iam put-role-policy \
    --role-name "${ROLE_NAME}" \
    --policy-name "${ROLE_NAME}-policy" \
    --policy-document "${BUNDLE_POLICY}"

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] done — no changes made"
    exit 0
fi

ROLE_ARN=$(aws iam get-role --role-name "${ROLE_NAME}" --query 'Role.Arn' --output text)
echo ""
echo "CI role ARN: ${ROLE_ARN}"
echo ""
echo "Next, set repository variables (requires a repo admin / gh CLI logged in):"
echo "  gh variable set AWS_CI_ROLE_ARN --body '${ROLE_ARN}'"
echo "  gh variable set REAL_CLOUD_TESTS_ENABLED --body true"
echo "  gh variable set REAL_CLOUD_TESTS_AWS_ENABLED --body true"
for bundle in "${BUNDLE_LIST[@]}"; do
    case "${bundle}" in
        ec2-quorum-manager) VAR="REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED" ;;
        ca-cluster-node) VAR="REAL_CLOUD_TESTS_AWS_CA_CLUSTER_ENABLED" ;;
        ca-cluster-node-rpc-tls) VAR="REAL_CLOUD_TESTS_AWS_CA_CLUSTER_RPC_TLS_ENABLED" ;;
        *) continue ;;
    esac
    echo "  gh variable set ${VAR} --body true"
done
if [[ ",${BUNDLES}," == *",ec2-quorum-manager,"* ]]; then
    echo ""
    echo "Note: the ec2-quorum-manager bundle references role"
    echo "  arn:aws:iam::${ACCOUNT_ID}:role/${NODE_ROLE_NAME_DEFAULT}"
    echo "Make sure provision-quorum-test-node-role.sh has already been run"
    echo "(with default naming, or update policies/ec2-quorum-manager.json"
    echo "and re-run this script if you used --role-name to rename it)."
fi
