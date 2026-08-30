#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Creates the long-lived IAM identity a *developer* uses locally, scoped to
# exactly the same service bundles CI's OIDC role is scoped to.
#
# Why this exists at all, when provision-oidc-role.sh deliberately avoids
# long-lived keys: OIDC federation is only available to a GitHub Actions
# runner. A developer running the real-cloud suite or the perf-cloud
# harness from a workstation has no web identity to exchange, so the
# alternative to a scoped IAM user is the account root key — which is what
# this script exists to stop anyone reaching for. The trade is deliberate
# and narrow: a key that can start and stop EC2 instances in the shapes
# these tests use, and cannot touch IAM, billing, or any other account.
#
# The bundle files under policies/ are shared with provision-oidc-role.sh
# on purpose. If the two drifted, a developer would be debugging a
# permissions failure that CI does not have, or worse, would not hit one
# CI does.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/provision-developer-user.sh \
#       --bundles BUNDLE[,BUNDLE...] [--user-name NAME] [--bucket NAME] \
#       [--create-access-key] [--write-profile PROFILE] [--set-default] \
#       [--rotate] [--dry-run]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
POLICY_DIR="${REPO_ROOT}/scripts/ci-cloud-credentials/aws/policies"

BUNDLES=""
BUCKET=""
USER_NAME="kythira-dev"
CREATE_ACCESS_KEY=0
WRITE_PROFILE=""
SET_DEFAULT=0
ROTATE=0
DRY_RUN=0
REGION="${AWS_REGION:-us-west-2}"

usage() {
    cat <<'EOF'
Usage: provision-developer-user.sh [OPTIONS]

Creates (or updates) a least-privilege IAM user for local kythira
development, with an inline policy built from exactly the bundles given.
Safe to re-run: re-running with a bundle removed genuinely revokes that
bundle's permissions, because put-user-policy replaces the document.

Required:
  --bundles LIST          Comma-separated bundle names from policies/,
                           e.g. perf-cloud,ec2-quorum-manager,ami-build

Optional:
  --user-name NAME        default: kythira-dev
  --bucket NAME           S3 bucket the object-persistence bundle is scoped
                           to; defaults to kythira-ci-<account>
  --region REGION         region written into the profile (default: $AWS_REGION
                           or us-west-2)
  --create-access-key     Create an access key for the user. Refuses if the
                           user already has two (the AWS limit) unless
                           --rotate is given.
  --rotate                Delete the user's existing access keys first. Only
                           meaningful with --create-access-key.
  --write-profile PROFILE Write the new key into ~/.aws/credentials under
                           [PROFILE] and ~/.aws/config under [profile PROFILE].
                           Implies --create-access-key.
  --set-default           Also write it as the [default] profile. Implies
                           --write-profile if none was given.
  --dry-run               Print the AWS CLI calls without executing them
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundles) BUNDLES="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --user-name) USER_NAME="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --create-access-key) CREATE_ACCESS_KEY=1; shift ;;
        --rotate) ROTATE=1; shift ;;
        --write-profile) WRITE_PROFILE="$2"; CREATE_ACCESS_KEY=1; shift 2 ;;
        --set-default) SET_DEFAULT=1; CREATE_ACCESS_KEY=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${BUNDLES}" ]]; then
    echo "ERROR: --bundles is required." >&2
    usage >&2
    exit 1
fi
if [[ "${SET_DEFAULT}" == "1" && -z "${WRITE_PROFILE}" ]]; then
    WRITE_PROFILE="${USER_NAME}"
fi

run() {
    if [[ "${DRY_RUN}" == "1" ]]; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

echo "[step] Sanity check: local AWS credentials"
if ! CALLER=$(aws sts get-caller-identity --output json 2>/dev/null); then
    echo "ERROR: 'aws sts get-caller-identity' failed — configure local AWS" \
         "credentials with IAM admin rights before running this script." >&2
    exit 1
fi
ACCOUNT_ID=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["Account"])' <<<"${CALLER}")
CALLER_ARN=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["Arn"])' <<<"${CALLER}")
echo "  account: ${ACCOUNT_ID}"
echo "  caller:  ${CALLER_ARN}"
# Deliberately not an error. The whole point of this script is to be run
# once with an IAM-admin identity — often the account root — and then
# never again from the identity it creates, which holds no IAM actions.
if [[ "${CALLER_ARN}" == *":root" ]]; then
    echo "  note: running as the account root. That is expected for a first" \
         "run and is exactly what the user this script creates exists to" \
         "replace for day-to-day work."
fi

echo "[step] Ensure IAM user: ${USER_NAME}"
if aws iam get-user --user-name "${USER_NAME}" >/dev/null 2>&1; then
    echo "  already exists"
else
    run aws iam create-user \
        --user-name "${USER_NAME}" \
        --tags "Key=Project,Value=kythira" "Key=ManagedBy,Value=provision-developer-user.sh"
fi
# No permissions boundary is attached, for the same reason
# provision-oidc-role.sh attaches none: this identity holds no IAM-write
# action, so there is nothing for a boundary to constrain.

# One customer-managed policy per bundle, rather than the single inline
# document provision-oidc-role.sh puts on the role. That is not a style
# choice: an IAM *user*'s inline policy is capped at 2048 characters and
# this bundle set is about 5 KB, so an inline policy silently stops being
# an option the moment more than one or two bundles are selected. A
# managed policy gets 6144 characters each and a user may hold ten, which
# fits every bundle in policies/ with room to spare — and keeps each
# bundle separately attachable, detachable and auditable in the console.
echo "[step] Build one managed policy per bundle: ${BUNDLES}"
POLICY_PREFIX="kythira-dev-"
WANTED_ARNS=()
IFS=',' read -ra BUNDLE_LIST <<< "${BUNDLES}"
for bundle in "${BUNDLE_LIST[@]}"; do
    POLICY_FILE="${POLICY_DIR}/${bundle}.json"
    if [[ ! -f "${POLICY_FILE}" ]]; then
        echo "ERROR: unknown bundle '${bundle}' — no ${POLICY_FILE}" >&2
        echo "       valid bundles: $(ls "${POLICY_DIR}" | sed 's/\.json$//' | tr '\n' ' ')" >&2
        exit 1
    fi
    if grep -q '{{BUCKET}}' "${POLICY_FILE}"; then
        if [[ -z "${BUCKET}" ]]; then
            BUCKET="kythira-ci-${ACCOUNT_ID}"
            echo "  --bucket not given; scoping '${bundle}' to the default bucket ${BUCKET}"
        else
            echo "  scoping '${bundle}' to bucket ${BUCKET}"
        fi
    fi
    BUNDLE_STATEMENTS=$(sed -e "s/{{ACCOUNT_ID}}/${ACCOUNT_ID}/g" \
                            -e "s/{{BUCKET}}/${BUCKET}/g" "${POLICY_FILE}")
    BUNDLE_DOC=$(python3 -c "
import json, sys
print(json.dumps({'Version': '2012-10-17', 'Statement': json.loads(sys.argv[1])}))
" "${BUNDLE_STATEMENTS}")
    if [[ ${#BUNDLE_DOC} -gt 6144 ]]; then
        echo "ERROR: bundle '${bundle}' renders to ${#BUNDLE_DOC} characters," \
             "over IAM's 6144-character managed policy limit." >&2
        exit 1
    fi

    POLICY_NAME="${POLICY_PREFIX}${bundle}"
    POLICY_ARN="arn:aws:iam::${ACCOUNT_ID}:policy/${POLICY_NAME}"
    WANTED_ARNS+=("${POLICY_ARN}")
    if aws iam get-policy --policy-arn "${POLICY_ARN}" >/dev/null 2>&1; then
        echo "  ${POLICY_NAME}: updating"
        # A managed policy holds at most five versions. Prune the
        # non-default ones before adding another, or the sixth update of
        # a long-lived policy fails with LimitExceeded — a failure that
        # only ever shows up months after this script was written.
        if [[ "${DRY_RUN}" != "1" ]]; then
            for v in $(aws iam list-policy-versions --policy-arn "${POLICY_ARN}" \
                       --query 'Versions[?!IsDefaultVersion].VersionId' --output text); do
                aws iam delete-policy-version --policy-arn "${POLICY_ARN}" --version-id "${v}"
            done
        fi
        run aws iam create-policy-version --policy-arn "${POLICY_ARN}" \
            --policy-document "${BUNDLE_DOC}" --set-as-default >/dev/null
    else
        echo "  ${POLICY_NAME}: creating"
        run aws iam create-policy --policy-name "${POLICY_NAME}" \
            --policy-document "${BUNDLE_DOC}" \
            --description "kythira ${bundle} bundle - scripts/ci-cloud-credentials/aws/policies/${bundle}.json" \
            >/dev/null
    fi
    run aws iam attach-user-policy --user-name "${USER_NAME}" --policy-arn "${POLICY_ARN}"
done

# Detaching what is no longer wanted is the half that makes "re-run with a
# bundle removed" actually revoke it. Only policies this script owns (the
# name prefix) are considered, so an operator's own attachment is left
# alone.
echo "[step] Detach bundles no longer selected"
if [[ "${DRY_RUN}" != "1" ]]; then
    for arn in $(aws iam list-attached-user-policies --user-name "${USER_NAME}" \
                 --query "AttachedPolicies[?starts_with(PolicyName, '${POLICY_PREFIX}')].PolicyArn" \
                 --output text); do
        keep=0
        for wanted in "${WANTED_ARNS[@]}"; do
            [[ "${arn}" == "${wanted}" ]] && keep=1 && break
        done
        if [[ "${keep}" == "0" ]]; then
            echo "  detaching ${arn}"
            aws iam detach-user-policy --user-name "${USER_NAME}" --policy-arn "${arn}"
        fi
    done
fi

# sts:GetCallerIdentity so the identity can name itself, and the two
# self-service key actions so a rotation does not need the root key back:
# both are scoped to this user's own ARN and to nothing else. This one
# fits inline comfortably and is the same for every bundle selection.
SELF_POLICY=$(python3 -c "
import json, sys
account, user = sys.argv[1], sys.argv[2]
print(json.dumps({'Version': '2012-10-17', 'Statement': [
    {'Sid': 'StsGetCallerIdentity', 'Effect': 'Allow',
     'Action': 'sts:GetCallerIdentity', 'Resource': '*'},
    {'Sid': 'SelfServiceKeyRotation', 'Effect': 'Allow',
     'Action': ['iam:ListAccessKeys', 'iam:CreateAccessKey',
                'iam:DeleteAccessKey', 'iam:UpdateAccessKey',
                'iam:GetAccessKeyLastUsed'],
     'Resource': 'arn:aws:iam::%s:user/%s' % (account, user)},
]}))
" "${ACCOUNT_ID}" "${USER_NAME}")

echo "[step] Ensure inline self-service policy: ${USER_NAME}-self"
run aws iam put-user-policy \
    --user-name "${USER_NAME}" \
    --policy-name "${USER_NAME}-self" \
    --policy-document "${SELF_POLICY}"

if [[ "${CREATE_ACCESS_KEY}" != "1" ]]; then
    echo ""
    echo "Done. No access key created (pass --create-access-key or --write-profile)."
    exit 0
fi

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] would create an access key for ${USER_NAME}"
    echo "[dry-run] done — no changes made"
    exit 0
fi

EXISTING_KEYS=$(aws iam list-access-keys --user-name "${USER_NAME}" \
    --query 'AccessKeyMetadata[].AccessKeyId' --output text)
if [[ -n "${EXISTING_KEYS}" && "${EXISTING_KEYS}" != "None" ]]; then
    if [[ "${ROTATE}" == "1" ]]; then
        for key in ${EXISTING_KEYS}; do
            echo "[step] Deleting existing access key ${key}"
            aws iam delete-access-key --user-name "${USER_NAME}" --access-key-id "${key}"
        done
    else
        COUNT=$(wc -w <<<"${EXISTING_KEYS}")
        if [[ "${COUNT}" -ge 2 ]]; then
            echo "ERROR: ${USER_NAME} already has ${COUNT} access keys, which is the" \
                 "AWS limit. Re-run with --rotate to replace them." >&2
            exit 1
        fi
        echo "  note: ${USER_NAME} already has ${COUNT} access key(s); adding another."
    fi
fi

echo "[step] Create access key"
KEY_JSON=$(aws iam create-access-key --user-name "${USER_NAME}" --output json)
KEY_ID=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["AccessKey"]["AccessKeyId"])' <<<"${KEY_JSON}")
KEY_SECRET=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["AccessKey"]["SecretAccessKey"])' <<<"${KEY_JSON}")
echo "  created ${KEY_ID}"

if [[ -z "${WRITE_PROFILE}" ]]; then
    echo ""
    echo "AWS_ACCESS_KEY_ID=${KEY_ID}"
    echo "AWS_SECRET_ACCESS_KEY=${KEY_SECRET}"
    echo ""
    echo "This secret is shown once and AWS does not store it. Save it now."
    exit 0
fi

# Written with python rather than >> so that re-running replaces the
# profile's keys instead of appending a second [profile] section that the
# AWS SDKs resolve by *first* occurrence — an append would silently keep
# using the revoked key.
write_profile() {
    local file="$1" section="$2"
    shift 2
    python3 - "$file" "$section" "$@" <<'PY'
import configparser, os, sys
path, section = sys.argv[1], sys.argv[2]
pairs = dict(kv.split('=', 1) for kv in sys.argv[3:])
cp = configparser.RawConfigParser()
cp.optionxform = str
if os.path.exists(path):
    cp.read(path)
if not cp.has_section(section):
    cp.add_section(section)
for k, v in pairs.items():
    cp.set(section, k, v)
os.makedirs(os.path.dirname(path), exist_ok=True)
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, 'w') as fh:
    cp.write(fh)
PY
}

CRED_FILE="${HOME}/.aws/credentials"
CONF_FILE="${HOME}/.aws/config"
BACKUP="${CRED_FILE}.bak.$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -f "${CRED_FILE}" ]]; then
    cp -p "${CRED_FILE}" "${BACKUP}"
    echo "[step] Backed up ${CRED_FILE} to ${BACKUP}"
fi

echo "[step] Writing profile [${WRITE_PROFILE}]"
write_profile "${CRED_FILE}" "${WRITE_PROFILE}" \
    "aws_access_key_id=${KEY_ID}" "aws_secret_access_key=${KEY_SECRET}"
write_profile "${CONF_FILE}" "profile ${WRITE_PROFILE}" \
    "region=${REGION}" "output=json"

if [[ "${SET_DEFAULT}" == "1" ]]; then
    echo "[step] Writing profile [default]"
    write_profile "${CRED_FILE}" "default" \
        "aws_access_key_id=${KEY_ID}" "aws_secret_access_key=${KEY_SECRET}"
    write_profile "${CONF_FILE}" "default" "region=${REGION}" "output=json"
fi

echo ""
echo "[step] Verify the new identity"
# AWS is eventually consistent on new access keys; a first call can return
# InvalidClientTokenId for a few seconds. Retrying is the difference
# between "the policy is wrong" and "the key is not there yet".
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    # env -u, not AWS_PROFILE=: the CLI treats an *empty* AWS_PROFILE as a
    # profile literally named "", and fails with "The config profile ()
    # could not be found" rather than falling back to the key pair in the
    # environment. The variable has to be absent, not blank.
    if OUT=$(env -u AWS_PROFILE -u AWS_SESSION_TOKEN \
             AWS_ACCESS_KEY_ID="${KEY_ID}" AWS_SECRET_ACCESS_KEY="${KEY_SECRET}" \
             aws sts get-caller-identity --output json 2>&1); then
        python3 -c 'import json,sys; d=json.load(sys.stdin); print("  " + d["Arn"])' <<<"${OUT}"
        break
    fi
    if [[ "${attempt}" == "10" ]]; then
        echo "ERROR: the new key never authenticated: ${OUT}" >&2
        exit 1
    fi
    sleep 3
done

echo ""
echo "Done. [${WRITE_PROFILE}]${SET_DEFAULT:+ and [default]} now use ${KEY_ID}."
echo "The account root key is no longer needed for day-to-day work; keep it"
echo "for IAM changes only."
