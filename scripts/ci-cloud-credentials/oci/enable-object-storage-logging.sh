#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Enables Object Storage **data-plane** event logging on the CI bucket, so that
# the `404 BucketNotFound` flake can be investigated from evidence instead of
# from re-runs.
#
# .kiro/specs/cloud-object-persistence/ task 19; spike-notes.md Finding 23.
#
# WHY THIS SCRIPT EXISTS. Finding 23's investigation opens with "take an
# opc-request-id from a live decline to the compartment audit log and read what
# the authorization decision actually says". That step has been owed since the
# flake was first measured and has never once been performable, because
# `oci audit event list` returns nothing over the failure window: Object
# Storage **data** events (GET/PUT/DELETE/ListObjects on objects) are not
# audited by default. Only control-plane events (create/delete bucket) are.
# This script turns on the missing half.
#
# WHAT THIS SCRIPT DELIBERATELY DOES NOT DO: touch a single IAM policy.
#
# The leading hypothesis for the flake is that tenancy policy
# `kythira-ci-launch-tags` carries `where target.tag-namespace.name =
# 'Oracle-Tags'` on the `kythira-ci` group, and that an *inapplicable*
# condition variable declines Object Storage requests rather than merely
# failing to match. Removing that clause is step 3 of Finding 23's sequence,
# and it is a human decision taken *after* steps 1 and 2 have produced a
# before-picture -- because mutating this tenancy's policies is exactly what
# broke this compartment on August 12, 2026, and the breakage masqueraded as a
# bucket outage. A script that helpfully "fixed" the policy while you were
# looking at it would destroy the very baseline it was run to establish.
# See policies/object-persistence.txt and policies/heartbeat.txt.
#
# LOGGING IS PROSPECTIVE. Enabling a log does not backfill it. Nothing here
# will explain the August failures; it makes the *next* decline readable. The
# order is therefore: run this, then generate fresh declines (RUNBOOK step 2),
# then read them. Running this and immediately searching for old request IDs
# will find nothing and prove nothing.
#
# Usage:
#   scripts/ci-cloud-credentials/oci/enable-object-storage-logging.sh \
#       --compartment-id OCID [--bucket NAME] [--log-group NAME]
#       [--categories read,write] [--retention-days 30] [--dry-run]
#
#   --bucket defaults to kythira-ci-artifacts, the bucket the
#   object-persistence and heartbeat bundles share.
set -euo pipefail

COMPARTMENT_ID=""
BUCKET="kythira-ci-artifacts"
LOG_GROUP="kythira-ci-logs"
CATEGORIES="read,write"
RETENTION_DAYS=30
DRY_RUN=0

usage() {
    # Print this file's leading comment block as help text. Deliberately NOT a
    # `sed -n 'A,Bp'` line range: that idiom was used here until the SPDX and
    # copyright header was added above, at which point every range silently
    # shifted by three lines and `--help` began printing the licence and
    # dropping its own last three lines. Match on content instead, so the
    # header can grow again without breaking this.
    awk '
        NR==1 && /^#!/                  { next }   # shebang
        /^# *Copyright \(c\)/           { next }   # licence header
        /^# *SPDX-License-Identifier:/  { next }
        /^#/    { line = $0; sub(/^# ?/, "", line); print line; started = 1; next }
        /^[[:space:]]*$/ { if (started) print ""; next }
        { exit }                                   # first code line ends the block
    ' "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compartment-id) COMPARTMENT_ID="$2"; shift 2 ;;
        --bucket)         BUCKET="$2";         shift 2 ;;
        --log-group)      LOG_GROUP="$2";      shift 2 ;;
        --categories)     CATEGORIES="$2";     shift 2 ;;
        --retention-days) RETENTION_DAYS="$2"; shift 2 ;;
        --dry-run)        DRY_RUN=1;           shift ;;
        -h|--help)        usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

[[ -n "${COMPARTMENT_ID}" ]] || { echo "--compartment-id is required" >&2; usage 1; }

command -v oci >/dev/null || { echo "the OCI CLI is not on PATH" >&2; exit 1; }

echo "compartment    ${COMPARTMENT_ID}"
echo "bucket         ${BUCKET}"
echo "log group      ${LOG_GROUP}"
echo "categories     ${CATEGORIES}"
echo "retention      ${RETENTION_DAYS} days"
echo

# ---------------------------------------------------------------------------
# Verify the category names against the service rather than trusting them.
#
# "read" and "write" are what the Logging service documents and what the
# Terraform provider emits, but a category that does not exist is accepted at
# the CLI and produces a log object that never receives an entry -- a silent
# no-op that would read as "logging is on, and the flake produces no events",
# which is the single most misleading outcome this investigation could reach.
# So: if the service tells us the category list, a bad name is fatal. If it
# will not (older CLI, restricted read), say so and continue -- failing to
# verify is not the same as verifying a contradiction.
# ---------------------------------------------------------------------------
#
# THE QUERY BELOW IS LOAD-BEARING AND WAS WRONG ON FIRST EXECUTION
# (2026-08-21). `categories` is NOT a member of the service object -- it is
# nested under `resource-types[]` (objectstorage's resource type is `bucket`).
# The original `data[?id==\`objectstorage\`].categories[].name` therefore
# returned `[]`, which this block reads as "the service would not tell us" and
# continues WITHOUT verifying. So the guard against a silently-never-filling
# log was itself a silent no-op -- the exact failure mode its own comment
# above warns about. Note also that `resource-types` must be double-quoted
# inside the single-quoted JMESPath: an unquoted hyphenated key is a parse
# error, not an empty result.
#
# `oci logging service list` also 401s intermittently in this tenancy, so a
# single failed call is not evidence that the service withholds the list;
# retry before concluding that.
AVAILABLE=""
for _attempt in 1 2 3; do
    if AVAILABLE="$(oci logging service list --all \
                      --query 'data[?id==`objectstorage`]."resource-types"[].categories[].name' \
                      --raw-output 2>/dev/null)" \
       && [[ -n "${AVAILABLE}" && "${AVAILABLE}" != "null" && "${AVAILABLE}" != "[]" ]]; then
        break
    fi
    sleep 3
done
if [[ -n "${AVAILABLE}" ]]; then
    if [[ -n "${AVAILABLE}" && "${AVAILABLE}" != "null" && "${AVAILABLE}" != "[]" ]]; then
        echo "objectstorage categories reported by the service: ${AVAILABLE}"
        IFS=',' read -ra WANT <<< "${CATEGORIES}"
        for c in "${WANT[@]}"; do
            if [[ "${AVAILABLE}" != *"\"${c}\""* && "${AVAILABLE}" != *"${c}"* ]]; then
                echo >&2
                echo "category '${c}' is not offered for objectstorage." >&2
                echo "available: ${AVAILABLE}" >&2
                echo "Refusing: a nonexistent category creates a log that silently never fills." >&2
                exit 1
            fi
        done
        echo "  all requested categories exist"
    else
        echo "NOTE: the service returned no category list; category names NOT verified."
    fi
else
    echo "NOTE: could not query 'oci logging service list'; category names NOT verified."
fi
echo

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "--dry-run: nothing created."
    echo "Would ensure log group '${LOG_GROUP}' and one SERVICE log per category"
    echo "on bucket '${BUCKET}', each with configuration:"
    echo '  {"compartmentId":"'"${COMPARTMENT_ID}"'","source":{"sourceType":"OCISERVICE",'
    echo '   "service":"objectstorage","resource":"'"${BUCKET}"'","category":"<category>"}}'
    exit 0
fi

# ---- log group (idempotent) ------------------------------------------------
LOG_GROUP_ID="$(oci logging log-group list --compartment-id "${COMPARTMENT_ID}" \
    --display-name "${LOG_GROUP}" --query 'data[0].id' --raw-output 2>/dev/null || true)"
if [[ -z "${LOG_GROUP_ID}" || "${LOG_GROUP_ID}" == "null" ]]; then
    echo "creating log group ${LOG_GROUP}"
    LOG_GROUP_ID="$(oci logging log-group create --compartment-id "${COMPARTMENT_ID}" \
        --display-name "${LOG_GROUP}" \
        --description "kythira CI service logs (cloud-object-persistence task 19)" \
        --wait-for-state SUCCEEDED --query 'data.resources[0].identifier' --raw-output)"
else
    echo "log group ${LOG_GROUP} already exists"
fi
echo "  ${LOG_GROUP_ID}"

# ---- one service log per category (idempotent) -----------------------------
# Object Storage permits exactly one log per (service, resource, category), and
# that uniqueness is TENANCY-WIDE, not per log group.
#
# FIRST EXECUTION, 2026-08-21: this block checked `--display-name` inside
# ${LOG_GROUP} only, found nothing, tried to create, and took a
# `409 Conflict -- A log already exists for this combination of service,
# resource, and category` from a log of a DIFFERENT name in a DIFFERENT group.
# The script then died mid-loop, leaving a freshly created but empty log group
# behind. Display name is not the key the service enforces; the combination is.
# So: search every log group in the compartment for the combination itself.
find_existing_log() {  # $1 = category; echoes "<group-ocid> <log-ocid>" if found
    local category="$1" grp
    for grp in $(oci logging log-group list --compartment-id "${COMPARTMENT_ID}" \
                   --all --query 'data[].id' --raw-output 2>/dev/null \
                 | tr -d '[],"' ); do
        [[ -n "${grp}" ]] || continue
        local hit
        hit="$(oci logging log list --log-group-id "${grp}" --all \
                 --query "data[?\"log-type\"=='SERVICE'
                            && configuration.source.service=='objectstorage'
                            && configuration.source.resource=='${BUCKET}'
                            && configuration.source.category=='${category}'].id | [0]" \
                 --raw-output 2>/dev/null || true)"
        if [[ -n "${hit}" && "${hit}" != "null" ]]; then
            echo "${grp} ${hit}"
            return 0
        fi
    done
    return 1
}

IFS=',' read -ra WANT <<< "${CATEGORIES}"
for category in "${WANT[@]}"; do
    display="${BUCKET}-${category}"
    if found="$(find_existing_log "${category}")"; then
        found_grp="${found%% *}"; found_log="${found##* }"
        echo "log for objectstorage/${BUCKET}/${category} already exists"
        echo "  ${found_log}"
        if [[ "${found_grp}" != "${LOG_GROUP_ID}" ]]; then
            echo "  NOTE: it lives in log group ${found_grp}, not ${LOG_GROUP}."
            echo "        That is the combination the service enforces, so nothing"
            echo "        is created here. Delete it first if you want it moved."
        fi
        continue
    fi
    echo "creating ${category} log on ${BUCKET}"
    config="$(python3 - "$COMPARTMENT_ID" "$BUCKET" "$category" <<'PY'
import json, sys
compartment, bucket, category = sys.argv[1], sys.argv[2], sys.argv[3]
print(json.dumps({
    "compartmentId": compartment,
    "source": {
        "sourceType": "OCISERVICE",
        "service": "objectstorage",
        "resource": bucket,
        "category": category,
    },
}))
PY
)"
    # NOT `--wait-for-state`: this tenancy's Logging control plane intermittently
    # answers `404 NotAuthorizedOrNotFound` when POLLING the work request, which
    # made this script exit 1 on 2026-08-21 for two logs it had just created
    # successfully. Reporting failure for work that succeeded is worse than
    # slow, so create without waiting and then confirm by looking for the
    # resource itself.
    oci logging log create --log-group-id "${LOG_GROUP_ID}" \
        --display-name "${display}" --log-type SERVICE --is-enabled true \
        --retention-duration "${RETENTION_DAYS}" --configuration "${config}" \
        >/dev/null 2>&1 || true
    log_id=""
    for _try in 1 2 3 4 5 6 7 8 9 10; do
        sleep 3
        if found="$(find_existing_log "${category}")"; then log_id="${found##* }"; break; fi
    done
    if [[ -z "${log_id}" ]]; then
        echo "FAILED to create the ${category} log on ${BUCKET}, and it does not exist." >&2
        echo "Re-run: this script is idempotent and will skip whatever did get created." >&2
        exit 1
    fi
    echo "  ${log_id}"
done

cat <<EOF

Done. Object Storage ${CATEGORIES} events on ${BUCKET} are now logged, and
**no IAM policy was read or written**.

These logs start now. They say nothing about the August failures. Continue with
RUNBOOK-object-storage-404-flake.md, whose steps are ordered so that the
policy change is the LAST thing done and the first thing that can be undone:

  1. Generate fresh traffic and capture a decline's opc-request-id.
  2. Read that request id back out of this log:

       ./read-object-storage-log.sh <start-rfc3339> <end-rfc3339>

     The search scope needs OCIDs, all three of
     <compartment>/<logGroup-OCID>/<log-OCID> -- a log group NAME does not
     resolve, and the two-part form answers "No log sources found to be read".
     read-object-storage-log.sh carries them and retries the intermittent
     declines; prefer it over a hand-written search-logs call.

  3. Measure a baseline decline rate over a fixed burst BEFORE changing
     anything, so there is a number rather than a remembered 3-16%.
  4. Only then consider the kythira-ci-launch-tags 'where' clause, and
     re-verify EVERY principal/service pair the compartment serves -- not just
     Object Storage. That omission is what made the August 12 breakage look
     like a bucket outage.
EOF
