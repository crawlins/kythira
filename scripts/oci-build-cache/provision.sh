#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Provisions the OCI Object Storage bucket, IAM users, policies and customer
# secret keys that hold this repository's two build caches
# (.kiro/specs/oci-build-cache/ Requirement 2, design.md Component 1).
#
# One bucket holds both caches: vcpkg's per-port archives under
# vcpkg/<triplet>/ and sccache's objects under sccache/. Two IAM users reach
# it through the S3 Compatibility API -- kythira-build-cache-rw for pushes to
# main, kythira-build-cache-ro for everything else -- because the writer
# policy is enforced by IAM and not only by the workflow's choice of mode. A
# workflow bug that selected readwrite on a pull request is refused by the
# service.
#
# DRY RUN IS THE DEFAULT. Nothing is created without --apply. Every action is
# printed either way, in the order it would run, so a dry run is a plan.
#
# IDEMPOTENT. Re-running with --apply creates only what is missing. It does
# NOT mint new keys: an existing customer secret key cannot be re-read (OCI
# returns the secret exactly once, at creation), so re-running would otherwise
# quietly leave the repository holding a key nobody can use. --rotate is the
# explicit way to replace one.
#
# KEYS ARE NEVER WRITTEN TO DISK. They are printed to stdout once, with the
# `gh secret set` commands to paste. If you lose them, --rotate; there is no
# recovery path, by design.
#
# OBJECT_DELETE IS GRANTED TO NOBODY. Expiry is the lifecycle policy's job,
# so a leaked key cannot empty the cache -- only add to it, at worst.
#
# Usage:
#   provision.sh [--apply] [--rotate rw|ro|both] [--bucket NAME]
#                [--compartment-id OCID] [--region REGION] [--tenancy-id OCID]
#
#   --apply           create; without it, print the plan and exit 0
#   --rotate WHICH    replace that user's customer secret key (implies --apply)
#   --bucket          default: kythira-build-cache
#   --compartment-id  default: $OCI_CI_COMPARTMENT_ID
#   --region          default: $OCI_CI_REGION
#   --tenancy-id      default: derived by walking up from the compartment
#
# Requires the OCI CLI, authenticated as a principal that can create buckets
# in the compartment and users, groups and policies in the tenancy. This is
# operator work, run by hand; CI never runs it.

set -euo pipefail

BUCKET="kythira-build-cache"
COMPARTMENT_ID="${OCI_CI_COMPARTMENT_ID:-}"
REGION="${OCI_CI_REGION:-}"
TENANCY_ID=""
APPLY=0
ROTATE=""

GROUP_NAME="kythira-build-cache"
RW_USER="kythira-build-cache-rw"
RO_USER="kythira-build-cache-ro"
SPEC_TAG_KEY="kythira-spec"
SPEC_TAG_VALUE="oci-build-cache"

usage() {
    # Print this file's leading comment block as help text. Deliberately NOT a
    # `sed -n 'A,Bp'` line range: that idiom breaks silently the moment the
    # licence header above changes length, printing the licence and dropping
    # the last lines of the real help. Match on content instead.
    awk '
        NR==1 && /^#!/                  { next }
        /^# *Copyright \(c\)/           { next }
        /^# *SPDX-License-Identifier:/  { next }
        /^#/    { line = $0; sub(/^# ?/, "", line); print line; started = 1; next }
        /^[[:space:]]*$/ { if (started) print ""; next }
        { exit }
    ' "${BASH_SOURCE[0]}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply)          APPLY=1; shift ;;
        --rotate)         ROTATE="${2:?--rotate needs rw, ro or both}"; APPLY=1; shift 2 ;;
        --bucket)         BUCKET="${2:?--bucket needs a name}"; shift 2 ;;
        --compartment-id) COMPARTMENT_ID="${2:?--compartment-id needs an OCID}"; shift 2 ;;
        --region)         REGION="${2:?--region needs a region}"; shift 2 ;;
        --tenancy-id)     TENANCY_ID="${2:?--tenancy-id needs an OCID}"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *)                echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$ROTATE" in ""|rw|ro|both) ;; *) echo "--rotate takes rw, ro or both" >&2; exit 2 ;; esac

command -v oci >/dev/null 2>&1 || { echo "error: the OCI CLI is not on PATH" >&2; exit 1; }
command -v jq  >/dev/null 2>&1 || { echo "error: jq is not on PATH" >&2; exit 1; }
[ -n "$COMPARTMENT_ID" ] || { echo "error: --compartment-id or \$OCI_CI_COMPARTMENT_ID is required" >&2; exit 2; }
[ -n "$REGION" ]         || { echo "error: --region or \$OCI_CI_REGION is required" >&2; exit 2; }

if [ "$APPLY" -eq 0 ]; then
    echo "DRY RUN — nothing will be created. Re-run with --apply to act."
else
    echo "APPLYING — creating what is missing in compartment $COMPARTMENT_ID"
fi
echo

# Runs an OCI CLI call, or prints it when this is a dry run. Every mutation in
# this script goes through here so that the dry run cannot drift from what
# --apply does: there is one code path and one place that decides to execute.
act() {
    if [ "$APPLY" -eq 0 ]; then
        printf '  would run: '; printf '%q ' "$@"; printf '\n'
        return 0
    fi
    "$@"
}

# A read-only query whose failure is not fatal: used to decide whether a thing
# already exists. `oci ... get` exits non-zero for "not found", which is the
# answer this wants, not an error.
exists() { "$@" >/dev/null 2>&1; }

# ── Namespace ────────────────────────────────────────────────────────────────
# The Object Storage namespace is a property of the tenancy and is part of the
# S3-compatibility endpoint host, which is why it becomes a repository
# variable rather than being derived in the workflow: deriving it would need
# the OCI CLI on every runner, which is exactly what x-aws avoids.
NAMESPACE=$(oci os ns get --query data --raw-output)
echo "Object Storage namespace: $NAMESPACE"
echo "S3 compatibility endpoint: https://${NAMESPACE}.compat.objectstorage.${REGION}.oraclecloud.com"
echo

# ── Tenancy ──────────────────────────────────────────────────────────────────
# IAM users, groups and policies live in the tenancy root, not in the
# compartment the bucket lives in. Walk up from the compartment rather than
# asking for another variable.
if [ -z "$TENANCY_ID" ]; then
    parent="$COMPARTMENT_ID"
    while :; do
        next=$(oci iam compartment get --compartment-id "$parent" \
                   --query 'data."compartment-id"' --raw-output 2>/dev/null || echo "")
        [ -n "$next" ] || break
        parent="$next"
    done
    TENANCY_ID="$parent"
fi
echo "Tenancy: $TENANCY_ID"
echo

# ── 1. Bucket ────────────────────────────────────────────────────────────────
# NoPublicAccess deliberately: reads are authenticated with the read-only key
# so that the read path and the write path are the same code path
# (Requirement 2.4). A public bucket would work and would make the read path
# untested, which is how a later public-ACL mistake turns into a write path.
echo "1. Bucket $BUCKET"
if exists oci os bucket get --bucket-name "$BUCKET"; then
    echo "  exists"
else
    act oci os bucket create \
        --compartment-id "$COMPARTMENT_ID" \
        --name "$BUCKET" \
        --public-access-type NoPublicAccess \
        --storage-tier Standard \
        --versioning Disabled \
        --freeform-tags "{\"${SPEC_TAG_KEY}\":\"${SPEC_TAG_VALUE}\"}"
fi
echo

# ── 2. Lifecycle ─────────────────────────────────────────────────────────────
# The asymmetry is deliberate (Requirement 2.2): a vcpkg archive is minutes of
# build per object and there are a few hundred; an sccache object is seconds
# and there are tens of thousands. Deleting the cheap ones sooner keeps the
# storage line flat without ever making a port rebuild.
echo "2. Lifecycle rules (sccache/ 30 days, vcpkg/ 90 days)"
LIFECYCLE_JSON=$(cat <<'JSON'
{"items": [
  {"name": "expire-sccache-objects", "action": "DELETE", "time-amount": 30,
   "time-unit": "DAYS", "is-enabled": true,
   "object-name-filter": {"inclusion-prefixes": ["sccache/"]}},
  {"name": "expire-vcpkg-archives", "action": "DELETE", "time-amount": 90,
   "time-unit": "DAYS", "is-enabled": true,
   "object-name-filter": {"inclusion-prefixes": ["vcpkg/"]}}
]}
JSON
)
if [ "$APPLY" -eq 0 ]; then
    printf '%s\n' "$LIFECYCLE_JSON" | sed 's/^/  would put: /'
else
    # --force because this is a PUT of the whole policy: re-running replaces
    # the two rules with the same two rules, which is what idempotent means
    # here. There is no partial update API for lifecycle rules.
    printf '%s' "$LIFECYCLE_JSON" | \
        oci os object-lifecycle-policy put --bucket-name "$BUCKET" --items file:///dev/stdin --force
fi
echo

# ── 3. Group and users ───────────────────────────────────────────────────────
echo "3. Group $GROUP_NAME and its two users"
# Every lookup is "list by name, take the id, empty if absent". OCI's list
# calls exit 0 with an empty data array when nothing matches, so the exit
# status says nothing and only the query result does — which is why none of
# these use `exists`.
id_of() {   # $1 = list subcommand (group|user), $2 = name
    local out
    out=$(oci iam "$1" list --compartment-id "$TENANCY_ID" --name "$2" \
              --query 'data[0].id' --raw-output 2>/dev/null || echo "")
    [ "$out" = "null" ] && out=""
    printf '%s' "$out"
}
group_id_of() { id_of group "$1"; }
user_id_of()  { id_of user  "$1"; }

group_id=$(group_id_of "$GROUP_NAME")
if [ -n "$group_id" ]; then
    echo "  group exists: $group_id"
else
    act oci iam group create --compartment-id "$TENANCY_ID" --name "$GROUP_NAME" \
        --description "Reaches the ${BUCKET} build cache over the S3 compatibility API" \
        --freeform-tags "{\"${SPEC_TAG_KEY}\":\"${SPEC_TAG_VALUE}\"}"
    # Re-resolve rather than parse the create output: on a dry run nothing was
    # created and this stays empty, which the membership step below checks.
    [ "$APPLY" -eq 1 ] && group_id=$(group_id_of "$GROUP_NAME")
fi

for user in "$RW_USER" "$RO_USER"; do
    uid=$(user_id_of "$user")
    if [ -n "$uid" ]; then
        echo "  user exists: $user ($uid)"
    else
        act oci iam user create --compartment-id "$TENANCY_ID" --name "$user" \
            --description "kythira build cache: ${user##*-} access to $BUCKET" \
            --freeform-tags "{\"${SPEC_TAG_KEY}\":\"${SPEC_TAG_VALUE}\"}"
        [ "$APPLY" -eq 1 ] && uid=$(user_id_of "$user")
    fi
    # Membership is checked separately from creation: a user that exists but
    # is not in the group has no access at all, and that is precisely the
    # state a half-finished earlier run leaves behind.
    if [ -z "$uid" ] || [ -z "$group_id" ]; then
        echo "  would add $user to $GROUP_NAME"
        continue
    fi
    member=$(oci iam group list-users --group-id "$group_id" \
                 --query "length(data[?id=='$uid'])" --raw-output 2>/dev/null || echo 0)
    if [ "$member" != "0" ]; then
        echo "  $user is already in $GROUP_NAME"
    else
        act oci iam group add-user --group-id "$group_id" --user-id "$uid"
    fi
done
echo

# ── 4. Policies ──────────────────────────────────────────────────────────────
# Scoped with `where target.bucket.name` so these users can reach this bucket
# and nothing else in the compartment. OBJECT_DELETE appears in neither
# statement; see the header.
echo "4. Policy kythira-build-cache-access"
POLICY_STATEMENTS=$(cat <<POLICY
Allow group ${GROUP_NAME} to read buckets in compartment id ${COMPARTMENT_ID} where target.bucket.name = '${BUCKET}'
Allow user ${RW_USER} to manage objects in compartment id ${COMPARTMENT_ID} where all { target.bucket.name = '${BUCKET}', any { request.permission = 'OBJECT_READ', request.permission = 'OBJECT_INSPECT', request.permission = 'OBJECT_CREATE', request.permission = 'OBJECT_OVERWRITE' } }
Allow user ${RO_USER} to read objects in compartment id ${COMPARTMENT_ID} where target.bucket.name = '${BUCKET}'
POLICY
)
printf '%s\n' "$POLICY_STATEMENTS" | sed 's/^/  /'
policy_id=$(oci iam policy list --compartment-id "$COMPARTMENT_ID" \
                --name kythira-build-cache-access --query 'data[0].id' --raw-output 2>/dev/null || echo "")
[ "$policy_id" = "null" ] && policy_id=""
if [ -n "$policy_id" ]; then
    echo "  policy exists ($policy_id) — the statements above are what it should"
    echo "  say. Not updated automatically: an IAM policy that CI depends on is"
    echo "  not something to overwrite from a script that cannot see why it was"
    echo "  last edited. Compare with: oci iam policy get --policy-id $policy_id"
else
    statements_json=$(printf '%s\n' "$POLICY_STATEMENTS" | jq -R . | jq -s .)
    if [ "$APPLY" -eq 0 ]; then
        echo "  would create policy kythira-build-cache-access with those 3 statements"
    else
        printf '%s' "$statements_json" | oci iam policy create \
            --compartment-id "$COMPARTMENT_ID" \
            --name kythira-build-cache-access \
            --description "kythira build cache access, .kiro/specs/oci-build-cache/" \
            --statements file:///dev/stdin \
            --freeform-tags "{\"${SPEC_TAG_KEY}\":\"${SPEC_TAG_VALUE}\"}"
    fi
fi
echo

# ── 5. Customer secret keys ──────────────────────────────────────────────────
# OCI returns the secret exactly once, at creation. That is why this step is
# not idempotent in the usual sense: it creates a key only when the user has
# none, or when --rotate names it. Printing goes to stdout and nowhere else.
echo "5. Customer secret keys"
mint_key() {
    local user="$1" which="$2" uid
    uid=$(user_id_of "$user")
    if [ -z "$uid" ] || [ "$uid" = "null" ]; then
        echo "  $user does not exist yet (dry run?) — skipping key"
        return 0
    fi
    local existing
    existing=$(oci iam customer-secret-key list --user-id "$uid" \
                   --query 'length(data)' --raw-output 2>/dev/null || echo 0)
    local rotating=0
    case "$ROTATE" in both) rotating=1 ;; "$which") rotating=1 ;; esac
    if [ "$existing" != "0" ] && [ "$rotating" -eq 0 ]; then
        echo "  $user already holds $existing key(s); not minting (pass --rotate $which to replace)"
        return 0
    fi
    if [ "$APPLY" -eq 0 ]; then
        echo "  would mint a customer secret key for $user"
        return 0
    fi
    local out
    out=$(oci iam customer-secret-key create --user-id "$uid" \
              --display-name "kythira-build-cache-$(date -u +%Y%m%d)")
    local key_id secret
    key_id=$(printf '%s' "$out" | jq -r '.data.id')
    secret=$(printf '%s' "$out" | jq -r '.data.key')
    echo
    echo "  ── $user — printed once, never written to disk ──"
    echo "  gh secret set OCI_BUILD_CACHE_${which^^}_ACCESS_KEY_ID --body '$key_id'"
    echo "  gh secret set OCI_BUILD_CACHE_${which^^}_SECRET_ACCESS_KEY --body '$secret'"
    echo
    if [ "$rotating" -eq 1 ] && [ "$existing" != "0" ]; then
        # Delete the old key only after the new one has been printed: a
        # rotation that deletes first and then fails to create leaves CI with
        # no credential and nothing on screen to recover from.
        local old
        old=$(oci iam customer-secret-key list --user-id "$uid" \
                  --query "data[?id!='$key_id'].id" --raw-output | jq -r '.[]?' 2>/dev/null || true)
        for k in $old; do
            echo "  deleting rotated-out key $k"
            oci iam customer-secret-key delete --user-id "$uid" --customer-secret-key-id "$k" --force
        done
    fi
}
mint_key "$RW_USER" rw
mint_key "$RO_USER" ro
echo

# ── 6. Repository configuration ──────────────────────────────────────────────
echo "6. Repository variables (set these once; they are not secrets)"
echo "  gh variable set OCI_BUILD_CACHE_BUCKET --body '$BUCKET'"
echo "  gh variable set OCI_BUILD_CACHE_NAMESPACE --body '$NAMESPACE'"
echo "  OCI_CI_REGION already exists and is reused ($REGION)"
echo
if [ "$APPLY" -eq 0 ]; then
    echo "Dry run complete. Nothing was created."
else
    echo "Done. Run scripts/oci-build-cache/audit.sh to see what now exists."
fi
