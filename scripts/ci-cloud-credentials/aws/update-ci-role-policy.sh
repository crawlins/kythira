#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Re-applies the CI role's inline bundle policy from policies/*.json, and
# nothing else. This is the policy half of provision-oidc-role.sh, split out
# so the reprovision-aws-ci-role workflow can run it under the narrow
# kythira-ci-role-updater identity (provision-ci-role-updater.sh), which may
# read and replace this one inline policy and cannot touch the role's trust
# policy, its permissions boundary or any other IAM object.
#
# By default it keeps exactly the bundles the role already carries (detected
# from the live policy's Sids), so a run that only picks up an edited bundle
# never silently revokes another one. It prints a statement-level diff before
# applying and reads the policy back afterwards.
#
# Usage:
#   scripts/ci-cloud-credentials/aws/update-ci-role-policy.sh \
#       [--bundles LIST] [--bucket NAME] [--role-name NAME] [--dry-run]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RENDER="${SCRIPT_DIR}/render-ci-policy.py"

BUNDLES=""
BUCKET=""
ROLE_NAME="kythira-ci-real-cloud-tests"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: update-ci-role-policy.sh [OPTIONS]

Replaces the CI role's inline policy with the rendered bundle policies.

Optional:
  --bundles LIST     Comma-separated bundles. Default: the bundles the live
                      policy already carries. Naming a list replaces the
                      policy wholesale, so a bundle left out is revoked.
  --bucket NAME      Bucket for the object-persistence bundle.
                      Default: kythira-ci-<account-id>
  --role-name NAME   default: kythira-ci-real-cloud-tests
  --dry-run          Show the diff without applying it
  -h, --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundles) BUNDLES="$2"; shift 2 ;;
        --bucket) BUCKET="$2"; shift 2 ;;
        --role-name) ROLE_NAME="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
POLICY_NAME="${ROLE_NAME}-policy"
BUCKET="${BUCKET:-kythira-ci-${ACCOUNT_ID}}"
echo "[step] account ${ACCOUNT_ID}, role ${ROLE_NAME}, policy ${POLICY_NAME}"

echo "[step] Read the live policy"
if aws iam get-role-policy --role-name "${ROLE_NAME}" --policy-name "${POLICY_NAME}" \
        --output json > "${WORK}/live.json"; then
    LIVE_BUNDLES=$(python3 "${RENDER}" --detect-bundles "${WORK}/live.json")
else
    echo "ERROR: could not read ${POLICY_NAME}. Provision the role with" \
         "provision-oidc-role.sh first." >&2
    exit 1
fi
echo "  bundles on the role now: ${LIVE_BUNDLES:-<none>}"
if [[ -z "${BUNDLES}" ]]; then
    BUNDLES="${LIVE_BUNDLES}"
fi
if [[ -z "${BUNDLES}" ]]; then
    echo "ERROR: no bundle detected on the live policy; pass --bundles." >&2
    exit 1
fi
echo "  bundles to apply:       ${BUNDLES}"

python3 "${RENDER}" --bundles "${BUNDLES}" --account-id "${ACCOUNT_ID}" \
    --bucket "${BUCKET}" > "${WORK}/new.json"

echo "[step] Diff, by statement (- live, + new)"
python3 - "${WORK}/live.json" "${WORK}/new.json" <<'EOF'
import json, sys
live = json.load(open(sys.argv[1]))["PolicyDocument"]
new = json.load(open(sys.argv[2]))
def stmts(doc):
    s = doc["Statement"]
    return {x.get("Sid", json.dumps(x, sort_keys=True)): x for x in (s if isinstance(s, list) else [s])}
old, cur = stmts(live), stmts(new)
changed = False
for sid in sorted(old.keys() | cur.keys()):
    if old.get(sid) == cur.get(sid):
        continue
    changed = True
    for sign, doc in (("-", old.get(sid)), ("+", cur.get(sid))):
        if doc is not None:
            print(f"{sign} {json.dumps(doc, sort_keys=True)}")
if not changed:
    print("  no change")
EOF

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] not applied"
    exit 0
fi

echo "[step] Apply"
aws iam put-role-policy --role-name "${ROLE_NAME}" --policy-name "${POLICY_NAME}" \
    --policy-document "file://${WORK}/new.json"

echo "[step] Verify"
aws iam get-role-policy --role-name "${ROLE_NAME}" --policy-name "${POLICY_NAME}" \
    --output json > "${WORK}/after.json"
python3 - "${WORK}/after.json" "${WORK}/new.json" <<'EOF'
import json, sys
after = json.load(open(sys.argv[1]))["PolicyDocument"]
if after != json.load(open(sys.argv[2])):
    sys.exit("ERROR: the policy read back differs from the one applied")
print("  the live policy matches the rendered one")
EOF
