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
AVAILABLE=""
if AVAILABLE="$(oci logging service list --all --query 'data[?id==`objectstorage`].categories[].name' \
                  --raw-output 2>/dev/null)"; then
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
# Object Storage permits exactly one read and one write access log per bucket,
# so a second create is an error rather than a duplicate; check first.
IFS=',' read -ra WANT <<< "${CATEGORIES}"
for category in "${WANT[@]}"; do
    display="${BUCKET}-${category}"
    existing="$(oci logging log list --log-group-id "${LOG_GROUP_ID}" \
        --display-name "${display}" --query 'data[0].id' --raw-output 2>/dev/null || true)"
    if [[ -n "${existing}" && "${existing}" != "null" ]]; then
        echo "log ${display} already exists"
        echo "  ${existing}"
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
    log_id="$(oci logging log create --log-group-id "${LOG_GROUP_ID}" \
        --display-name "${display}" --log-type SERVICE --is-enabled true \
        --retention-duration "${RETENTION_DAYS}" --configuration "${config}" \
        --wait-for-state SUCCEEDED --query 'data.resources[0].identifier' --raw-output)"
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

       oci logging-search search-logs --search-query \\
         "search \\"${COMPARTMENT_ID}/${LOG_GROUP}\\" | where data.opcRequestId = '<id>'" \\
         --time-start <ISO8601> --time-end <ISO8601>

  3. Measure a baseline decline rate over a fixed burst BEFORE changing
     anything, so there is a number rather than a remembered 3-16%.
  4. Only then consider the kythira-ci-launch-tags 'where' clause, and
     re-verify EVERY principal/service pair the compartment serves -- not just
     Object Storage. That omission is what made the August 12 breakage look
     like a bucket outage.
EOF
