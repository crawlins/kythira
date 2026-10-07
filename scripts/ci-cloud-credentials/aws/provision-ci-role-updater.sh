#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# One-time setup, run by an IAM admin, for the reprovision-aws-ci-role
# workflow. After it, re-applying the CI role's bundle policy is a workflow
# dispatch that waits for a required reviewer's approval, instead of a local
# run with admin credentials.
#
# Creates or updates:
#   1. kythira-ci-real-cloud-tests-boundary, a managed policy set as the CI
#      role's permissions boundary. It allows everything except IAM,
#      Organizations and account administration (plus iam:PassRole on the
#      quorum-test node role, the one IAM action a bundle needs). Whatever
#      inline policy the workflow applies, the CI role can never grant itself
#      or anyone else IAM rights.
#   2. kythira-ci-role-updater, an OIDC role trusted only by jobs in the
#      ci-role-admin GitHub Environment. It may read and replace the CI
#      role's one inline policy and nothing else: no trust-policy, boundary,
#      managed-policy or role changes.
#   3. The ci-role-admin environment, restricted to the main branch, with
#      the given required reviewers, and the AWS_CI_ROLE_UPDATER_ARN
#      repository variable (needs gh with admin rights on the repository).
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-ci-role-updater.sh \
#       --github-org ORG --github-repo REPO --reviewer LOGIN [--reviewer LOGIN...] \
#       [--ci-role-name NAME] [--environment NAME] [--dry-run]
set -euo pipefail

GITHUB_ORG=""
GITHUB_REPO=""
REVIEWERS=()
CI_ROLE_NAME="kythira-ci-real-cloud-tests"
UPDATER_ROLE_NAME="kythira-ci-role-updater"
ENVIRONMENT="ci-role-admin"
NODE_ROLE_NAME="kythira-aws-quorum-test-node-role"
OIDC_PROVIDER_URL="token.actions.githubusercontent.com"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-ci-role-updater.sh [OPTIONS]

Sets up the approval-gated workflow that re-applies the AWS CI role's
bundle policy. Run once, locally, with IAM admin credentials. Safe to re-run.

Required:
  --github-org ORG        e.g. crawlins
  --github-repo REPO      e.g. kythira
  --reviewer LOGIN        GitHub user who must approve each run (repeatable)

Optional:
  --ci-role-name NAME     default: kythira-ci-real-cloud-tests
  --environment NAME      default: ci-role-admin
  --dry-run               Print the calls that would change anything
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --github-org) GITHUB_ORG="$2"; shift 2 ;;
        --github-repo) GITHUB_REPO="$2"; shift 2 ;;
        --reviewer) REVIEWERS+=("$2"); shift 2 ;;
        --ci-role-name) CI_ROLE_NAME="$2"; shift 2 ;;
        --environment) ENVIRONMENT="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${GITHUB_ORG}" || -z "${GITHUB_REPO}" || ${#REVIEWERS[@]} -eq 0 ]]; then
    echo "ERROR: --github-org, --github-repo and at least one --reviewer are required." >&2
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
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
echo "  account: ${ACCOUNT_ID}"
CI_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${CI_ROLE_NAME}"
aws iam get-role --role-name "${CI_ROLE_NAME}" >/dev/null || {
    echo "ERROR: ${CI_ROLE_NAME} does not exist; run provision-oidc-role.sh first." >&2
    exit 1
}
aws iam get-open-id-connect-provider \
    --open-id-connect-provider-arn "arn:aws:iam::${ACCOUNT_ID}:oidc-provider/${OIDC_PROVIDER_URL}" \
    >/dev/null || {
    echo "ERROR: the GitHub OIDC provider is missing; run provision-oidc-role.sh first." >&2
    exit 1
}

echo "[step] Permissions boundary for ${CI_ROLE_NAME}"
BOUNDARY_NAME="${CI_ROLE_NAME}-boundary"
BOUNDARY_ARN="arn:aws:iam::${ACCOUNT_ID}:policy/${BOUNDARY_NAME}"
# A boundary only caps; the inline policy still has to grant. NotAction
# keeps every service the bundles use (EC2, Auto Scaling, S3, CloudWatch
# Logs, SSM, STS) without listing them, so a new bundle needs no boundary
# change unless it wants IAM.
BOUNDARY=$(cat <<EOF
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Sid": "EverythingButAccountAdministration",
            "Effect": "Allow",
            "NotAction": ["iam:*", "organizations:*", "account:*"],
            "Resource": "*"
        },
        {
            "Sid": "PassTheQuorumTestNodeRoleOnly",
            "Effect": "Allow",
            "Action": "iam:PassRole",
            "Resource": "arn:aws:iam::${ACCOUNT_ID}:role/${NODE_ROLE_NAME}"
        }
    ]
}
EOF
)
if aws iam get-policy --policy-arn "${BOUNDARY_ARN}" >/dev/null 2>&1; then
    # The backticks below are JMESPath literals, not shell expansions.
    # shellcheck disable=SC2016
    # A managed policy keeps at most five versions; drop the oldest
    # non-default one before adding another.
    VERSIONS=$(aws iam list-policy-versions --policy-arn "${BOUNDARY_ARN}" \
        --query 'Versions[?IsDefaultVersion==`false`].VersionId' --output text)
    read -ra VERSION_LIST <<< "${VERSIONS}"
    if [[ ${#VERSION_LIST[@]} -ge 4 ]]; then
        # shellcheck disable=SC2016
        OLDEST=$(aws iam list-policy-versions --policy-arn "${BOUNDARY_ARN}" \
            --query 'sort_by(Versions[?IsDefaultVersion==`false`], &CreateDate)[0].VersionId' \
            --output text)
        run aws iam delete-policy-version --policy-arn "${BOUNDARY_ARN}" --version-id "${OLDEST}"
    fi
    run aws iam create-policy-version --policy-arn "${BOUNDARY_ARN}" \
        --policy-document "${BOUNDARY}" --set-as-default
else
    run aws iam create-policy --policy-name "${BOUNDARY_NAME}" \
        --policy-document "${BOUNDARY}" \
        --description "Caps ${CI_ROLE_NAME}: no IAM, Organizations or account administration"
fi
run aws iam put-role-permissions-boundary --role-name "${CI_ROLE_NAME}" \
    --permissions-boundary "${BOUNDARY_ARN}"

echo "[step] Updater role: ${UPDATER_ROLE_NAME}"
SUBJECT="repo:${GITHUB_ORG}/${GITHUB_REPO}:environment:${ENVIRONMENT}"
echo "  trusted subject: ${SUBJECT}"
TRUST=$(cat <<EOF
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Principal": {"Federated": "arn:aws:iam::${ACCOUNT_ID}:oidc-provider/${OIDC_PROVIDER_URL}"},
            "Action": "sts:AssumeRoleWithWebIdentity",
            "Condition": {
                "StringEquals": {
                    "${OIDC_PROVIDER_URL}:aud": "sts.amazonaws.com",
                    "${OIDC_PROVIDER_URL}:sub": "${SUBJECT}"
                }
            }
        }
    ]
}
EOF
)
UPDATER_POLICY=$(cat <<EOF
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Sid": "ReplaceTheCiRoleInlinePolicyOnly",
            "Effect": "Allow",
            "Action": ["iam:GetRole", "iam:GetRolePolicy", "iam:PutRolePolicy"],
            "Resource": "${CI_ROLE_ARN}"
        },
        {
            "Sid": "NeverLoosenTheCiRole",
            "Effect": "Deny",
            "Action": [
                "iam:DeleteRolePermissionsBoundary",
                "iam:PutRolePermissionsBoundary",
                "iam:UpdateAssumeRolePolicy",
                "iam:AttachRolePolicy",
                "iam:DeleteRolePolicy"
            ],
            "Resource": "*"
        }
    ]
}
EOF
)
if aws iam get-role --role-name "${UPDATER_ROLE_NAME}" >/dev/null 2>&1; then
    run aws iam update-assume-role-policy --role-name "${UPDATER_ROLE_NAME}" \
        --policy-document "${TRUST}"
else
    run aws iam create-role --role-name "${UPDATER_ROLE_NAME}" \
        --assume-role-policy-document "${TRUST}" --max-session-duration 3600 \
        --description "Re-applies ${CI_ROLE_NAME}'s inline policy from the reprovision-aws-ci-role workflow"
fi
run aws iam put-role-policy --role-name "${UPDATER_ROLE_NAME}" \
    --policy-name "${UPDATER_ROLE_NAME}-policy" --policy-document "${UPDATER_POLICY}"
UPDATER_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${UPDATER_ROLE_NAME}"

echo "[step] GitHub environment ${ENVIRONMENT} and repository variable"
REVIEWER_JSON=""
for login in "${REVIEWERS[@]}"; do
    id=$(gh api "users/${login}" --jq .id)
    REVIEWER_JSON+="${REVIEWER_JSON:+,}{\"type\":\"User\",\"id\":${id}}"
done
ENV_BODY="{\"reviewers\":[${REVIEWER_JSON}],\"prevent_self_review\":false,
  \"deployment_branch_policy\":{\"protected_branches\":false,\"custom_branch_policies\":true}}"
if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] gh api -X PUT repos/${GITHUB_ORG}/${GITHUB_REPO}/environments/${ENVIRONMENT} <<< ${ENV_BODY}"
else
    gh api -X PUT "repos/${GITHUB_ORG}/${GITHUB_REPO}/environments/${ENVIRONMENT}" \
        --input - <<< "${ENV_BODY}" >/dev/null
fi
if ! gh api "repos/${GITHUB_ORG}/${GITHUB_REPO}/environments/${ENVIRONMENT}/deployment-branch-policies" \
        --jq '.branch_policies[].name' 2>/dev/null | grep -qx main; then
    run gh api -X POST \
        "repos/${GITHUB_ORG}/${GITHUB_REPO}/environments/${ENVIRONMENT}/deployment-branch-policies" \
        -f name=main -f type=branch
fi
run gh variable set AWS_CI_ROLE_UPDATER_ARN --repo "${GITHUB_ORG}/${GITHUB_REPO}" \
    --body "${UPDATER_ARN}"

echo ""
echo "Done. Updater role: ${UPDATER_ARN}"
echo "Each 'Reprovision AWS CI role' run now waits for approval from: ${REVIEWERS[*]}"
