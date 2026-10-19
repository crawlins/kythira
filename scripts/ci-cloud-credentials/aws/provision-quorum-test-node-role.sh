#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

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
# --leader creates a second, separate role and profile instead
# (kythira-aws-chaos-node-role / -profile) for
# tests/aws_chaos_node_crash_real_ec2_test.cpp. There the Raft leader on an
# instance owns aws_ec2_quorum_manager, so the instance itself launches,
# tags and terminates EC2 instances. Those rights are kept off the plain
# node role above, which every other real-EC2 test launches with.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh \
#       [--leader] [--role-name NAME] [--profile-name NAME] [--dry-run]
set -euo pipefail

ROLE_NAME="kythira-aws-quorum-test-node-role"
PROFILE_NAME="kythira-aws-quorum-test-node-profile"
POLICY_NAME="kythira-aws-quorum-test-node-policy"
DRY_RUN=0
LEADER=0
ROLE_NAME_SET=0
PROFILE_NAME_SET=0

usage() {
    cat <<'EOF'
Usage: provision-quorum-test-node-role.sh [--leader] [OPTIONS]

Creates the static IAM role + instance profile that
tests/aws_quorum_manager_real_ec2_test.cpp's launched EC2 instances use.
Safe to re-run: every step checks for existence first.

Options:
  --leader              Create the chaos_node leader role instead
                        (kythira-aws-chaos-node-role / -profile), whose
                        instances may launch, tag and terminate instances
                        carrying kythira:managed-by=ec2_quorum_manager
  --role-name NAME      IAM role name (default: kythira-aws-quorum-test-node-role)
  --profile-name NAME   Instance profile name (default: kythira-aws-quorum-test-node-profile)
  --dry-run             Print the AWS CLI calls that would run without executing them
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --leader) LEADER=1; shift ;;
        --role-name) ROLE_NAME="$2"; ROLE_NAME_SET=1; shift 2 ;;
        --profile-name) PROFILE_NAME="$2"; PROFILE_NAME_SET=1; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ "${LEADER}" == "1" ]]; then
    [[ "${ROLE_NAME_SET}" == "1" ]] || ROLE_NAME="kythira-aws-chaos-node-role"
    [[ "${PROFILE_NAME_SET}" == "1" ]] || PROFILE_NAME="kythira-aws-chaos-node-profile"
    POLICY_NAME="kythira-aws-chaos-node-policy"
fi

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

if [[ "${LEADER}" == "1" ]]; then
    ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
    # What aws_ec2_quorum_manager calls from the leader: RunInstances,
    # CreateTags at launch, DescribeInstances, DescribeInstanceStatus and
    # TerminateInstances. The instance it launches must carry
    # kythira:managed-by=ec2_quorum_manager (the manager always sets it),
    # and only an instance carrying it can be terminated, the same scoping
    # the CI role's bundles use. PassRole lets a replacement launch with
    # this same profile, so a replacement that becomes leader can heal too.
    INLINE_POLICY='{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Sid": "Describe",
            "Effect": "Allow",
            "Action": ["ec2:DescribeInstances", "ec2:DescribeInstanceStatus", "sts:GetCallerIdentity"],
            "Resource": "*"
        },
        {
            "Sid": "LaunchManagedInstances",
            "Effect": "Allow",
            "Action": "ec2:RunInstances",
            "Resource": "arn:aws:ec2:*:*:instance/*",
            "Condition": {"StringEquals": {"aws:RequestTag/kythira:managed-by": "ec2_quorum_manager"}}
        },
        {
            "Sid": "LaunchSupportingResources",
            "Effect": "Allow",
            "Action": "ec2:RunInstances",
            "Resource": [
                "arn:aws:ec2:*:*:network-interface/*",
                "arn:aws:ec2:*:*:volume/*",
                "arn:aws:ec2:*:*:subnet/*",
                "arn:aws:ec2:*:*:security-group/*",
                "arn:aws:ec2:*:*:key-pair/*",
                "arn:aws:ec2:*::image/*"
            ]
        },
        {
            "Sid": "TagAtLaunchOnly",
            "Effect": "Allow",
            "Action": "ec2:CreateTags",
            "Resource": "*",
            "Condition": {"StringEquals": {"ec2:CreateAction": "RunInstances"}}
        },
        {
            "Sid": "TerminateManagedInstances",
            "Effect": "Allow",
            "Action": "ec2:TerminateInstances",
            "Resource": "arn:aws:ec2:*:*:instance/*",
            "Condition": {"StringEquals": {"aws:ResourceTag/kythira:managed-by": "ec2_quorum_manager"}}
        },
        {
            "Sid": "PassOwnRole",
            "Effect": "Allow",
            "Action": "iam:PassRole",
            "Resource": "arn:aws:iam::'"${ACCOUNT_ID}"':role/'"${ROLE_NAME}"'"
        }
    ]
}'
fi

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
        --description "kythira aws_quorum_manager_real_ec2_test node identity - .kiro/specs/ci-real-cloud-tests/"
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
