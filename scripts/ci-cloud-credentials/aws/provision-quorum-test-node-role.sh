#!/usr/bin/env bash
# Creates, once and idempotently, the static IAM role + instance profile
# that tests/aws_quorum_manager_real_ec2_test.cpp's launched EC2 instances
# use — separate from, and unrelated to, provision-oidc-role.sh (which
# creates the CI role's own identity). Run by an operator with full IAM
# rights; never run by CI itself.
#
# include/raft/aws_ec2_quorum_manager.hpp (the production code under test)
# never calls an IAM API — its iam_instance_profile config field is a
# plain string, referenced by name on RunInstances. This script exists so
# that name can point at a role/profile created once, in advance, instead
# of dynamically created and destroyed by the test on every run — which
# in turn is what lets the CI role's own permissions (see
# provision-oidc-role.sh) hold zero IAM-write actions.
#
# .kiro/specs/ci-real-cloud-tests/ Requirement 3 / design.md Component 3.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh \
#       [--role-name NAME] [--profile-name NAME] [--dry-run]
set -euo pipefail

ROLE_NAME="kythira-aws-quorum-test-node-role"
PROFILE_NAME="kythira-aws-quorum-test-node-profile"
POLICY_NAME="kythira-aws-quorum-test-node-policy"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-quorum-test-node-role.sh [OPTIONS]

Creates the static IAM role + instance profile that
tests/aws_quorum_manager_real_ec2_test.cpp's launched EC2 instances use.
Safe to re-run: every step checks for existence first.

Options:
  --role-name NAME      IAM role name (default: kythira-aws-quorum-test-node-role)
  --profile-name NAME   Instance profile name (default: kythira-aws-quorum-test-node-profile)
  --dry-run             Print the AWS CLI calls that would run without executing them
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --role-name) ROLE_NAME="$2"; shift 2 ;;
        --profile-name) PROFILE_NAME="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

run() {
    if [[ "${DRY_RUN}" == "1" ]]; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

echo "[step] Sanity check: local AWS credentials"
if ! aws sts get-caller-identity >/dev/null 2>&1; then
    echo "ERROR: 'aws sts get-caller-identity' failed — configure local AWS credentials" \
         "with IAM admin rights before running this script." >&2
    exit 1
fi

# Trust policy: EC2 instances assume this role — identical to
# tests/aws_quorum_manager_real_ec2_test.cpp's own create_iam_role().
TRUST_POLICY='{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Principal": {"Service": "ec2.amazonaws.com"},
            "Action": "sts:AssumeRole"
        }
    ]
}'

# Inline policy: sts:GetCallerIdentity only — identical in scope to what
# the test creates dynamically today. Nodes carrying this role need no
# other AWS permission; heartbeat tag writes and DescribeInstances are not
# performed by cluster nodes themselves.
INLINE_POLICY='{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Action": ["sts:GetCallerIdentity"],
            "Resource": "*"
        }
    ]
}'

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
        --description "kythira aws_quorum_manager_real_ec2_test node identity — .kiro/specs/ci-real-cloud-tests/"
fi

echo "[step] Ensure inline policy: ${POLICY_NAME}"
run aws iam put-role-policy \
    --role-name "${ROLE_NAME}" \
    --policy-name "${POLICY_NAME}" \
    --policy-document "${INLINE_POLICY}"

echo "[step] Ensure instance profile: ${PROFILE_NAME}"
if aws iam get-instance-profile --instance-profile-name "${PROFILE_NAME}" >/dev/null 2>&1; then
    echo "  already exists"
else
    run aws iam create-instance-profile --instance-profile-name "${PROFILE_NAME}"
    if [[ "${DRY_RUN}" != "1" ]]; then
        # IAM propagation delay before the profile can be used, matching
        # the existing test fixture's own create_iam_role().
        sleep 10
    fi
fi

echo "[step] Ensure role is attached to instance profile"
if aws iam get-instance-profile --instance-profile-name "${PROFILE_NAME}" 2>/dev/null \
        | grep -q "\"RoleName\": \"${ROLE_NAME}\""; then
    echo "  already attached"
else
    run aws iam add-role-to-instance-profile \
        --instance-profile-name "${PROFILE_NAME}" \
        --role-name "${ROLE_NAME}"
fi

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] done — no changes made"
    exit 0
fi

ROLE_ARN=$(aws iam get-role --role-name "${ROLE_NAME}" --query 'Role.Arn' --output text)
echo ""
echo "Node role ARN: ${ROLE_ARN}"
echo ""
echo "Next: pass this ARN's account ID into"
echo "  scripts/ci-cloud-credentials/aws/provision-oidc-role.sh --bundles ec2-quorum-manager,..."
echo "(that script substitutes it into policies/ec2-quorum-manager.json's"
echo "{{ACCOUNT_ID}} placeholder automatically, via its own 'aws sts get-caller-identity' call)."
