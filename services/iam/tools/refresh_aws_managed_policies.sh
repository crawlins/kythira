#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Rewrites services/iam/tools/aws_managed_policies.json from AWS: for each
# policy in POLICIES, reads its default version with `aws iam get-policy`
# and `aws iam get-policy-version`. Read-only and free; needs a profile allowed
# iam:GetPolicy and iam:GetPolicyVersion on arn:aws:iam::aws:policy/* (the CI
# role's permissions boundary denies every IAM action, so run it locally).
#
# Usage: services/iam/tools/refresh_aws_managed_policies.sh [--profile NAME]
set -euo pipefail

PROFILE_ARGS=()
if [[ "${1:-}" == "--profile" ]]; then
    PROFILE_ARGS=(--profile "$2")
fi

# Requirement 4.3 of .kiro/specs/iam-compatible-auth/, plus the ElastiCache
# policies Requirement 13 needs.
POLICIES=(
    AdministratorAccess
    ReadOnlyAccess
    IAMFullAccess
    IAMReadOnlyAccess
    AmazonElastiCacheFullAccess
    AmazonElastiCacheReadOnlyAccess
)

OUT="$(cd "$(dirname "$0")" && pwd)/aws_managed_policies.json"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

for name in "${POLICIES[@]}"; do
    arn="arn:aws:iam::aws:policy/${name}"
    aws "${PROFILE_ARGS[@]}" iam get-policy --policy-arn "${arn}" --output json \
        > "${TMP}/${name}.policy.json"
    version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["Policy"]["DefaultVersionId"])' \
        "${TMP}/${name}.policy.json")"
    aws "${PROFILE_ARGS[@]}" iam get-policy-version --policy-arn "${arn}" \
        --version-id "${version}" --output json > "${TMP}/${name}.version.json"
done

python3 - "${TMP}" "${OUT}" "${POLICIES[@]}" <<'PY'
import json, sys, urllib.parse
tmp, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
policies = []
for name in names:
    pol = json.load(open(f"{tmp}/{name}.policy.json"))["Policy"]
    ver = json.load(open(f"{tmp}/{name}.version.json"))["PolicyVersion"]
    doc = ver["Document"]
    if isinstance(doc, str):  # the CLI returns it URL-encoded in some versions
        doc = json.loads(urllib.parse.unquote(doc))
    policies.append({"name": name, "path": pol["Path"],
                     "version_id": ver["VersionId"], "document": doc})
with open(out) as f:
    head = json.load(f)
head["policies"] = policies
with open(out, "w") as f:
    json.dump(head, f, indent=2)
    f.write("\n")
print(f"wrote {len(policies)} policies to {out}")
PY
