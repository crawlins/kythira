#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Leak audit and sweep for aws_acm_pca_provider_real_test
# (.kiro/specs/acm-pca-ephemeral-ca/, task 5 and Requirement 3.2-3.3).
#
# Usage: aws-acm-pca-leaks.sh audit|sweep
#
#   audit  Lists every ACM Private CA tagged kythira:suite=acm-pca-real-test
#          that is not DELETED and was created more than
#          KYTHIRA_ACM_PCA_AUDIT_GRACE_SECONDS (default 300) ago. The grace
#          period keeps a run that is still going, or one whose CA was created
#          seconds ago, from reading as a leak. Exits 1 on a leak, and also
#          when it could not list, because an audit that cannot see is not an
#          audit that found nothing.
#
#   sweep  Deletes what the audit would report (the same grace period
#          applies, so a run still in progress keeps its CAs), in the suite's
#          own teardown order: an ACTIVE CA is disabled first (AWS refuses to
#          delete it otherwise), then every CA is deleted with the minimum
#          seven-day restore window. AWS deletes a CREATING or PENDING_CERTIFICATE CA
#          immediately. Exits 1 if any CA survives. Run it AFTER the audit,
#          so detection stays loud and remediation does not hide it.
#
# A DELETED CA is never reported: it sits in its restore window, still
# counting against the per-region CA quota, but it no longer bills.
#
# ListCertificateAuthorities has no server-side tag filter, so the script
# lists every CA and reads each non-DELETED one's tags. It acts only on CAs
# carrying the suite tag, so it never touches a CA another suite or an
# operator created, including one named by $KYTHIRA_TEST_ACM_PCA_ARN.
#
# Region comes from AWS_DEFAULT_REGION / AWS_REGION, credentials from the
# usual AWS CLI chain (AWS_PROFILE works for a local run).

set -euo pipefail

TAG_KEY="kythira:suite"
TAG_VALUE="acm-pca-real-test"
GRACE="${KYTHIRA_ACM_PCA_AUDIT_GRACE_SECONDS:-300}"

# CreatedAt as epoch seconds. The CLI prints it as an ISO 8601 string or as
# epoch seconds depending on its cli_timestamp_format; GNU date reads both,
# offsets included.
to_epoch() {
    case "$1" in
        *[!0-9.]*) date -d "$1" +%s ;;
        *) date -d "@$1" +%s ;;
    esac
}

# Prints a JSON array of the suite's non-DELETED CAs, each with its arn,
# status, creation time (epoch seconds) and run id, or fails if any call does.
# Never prints a partial list.
inventory() {
    local cas arn status created tags out
    # shellcheck disable=SC2016  # JMESPath literal backticks, not shell
    cas=$(aws acm-pca list-certificate-authorities \
        --query 'CertificateAuthorities[?Status!=`DELETED`].[Arn,Status,CreatedAt]' \
        --output json) || return 1
    out='[]'
    while IFS=$'\t' read -r arn status created; do
        [ -n "${arn}" ] || continue
        tags=$(aws acm-pca list-tags --certificate-authority-arn "${arn}" --output json) || return 1
        if ! jq -e --arg k "${TAG_KEY}" --arg v "${TAG_VALUE}" \
            'any(.Tags[]?; .Key == $k and .Value == $v)' <<<"${tags}" >/dev/null; then
            continue
        fi
        created=$(to_epoch "${created}") || return 1
        out=$(jq --arg arn "${arn}" --arg status "${status}" --argjson created "${created}" \
            --argjson tags "${tags}" \
            '. + [{arn: $arn, status: $status, created: $created,
                   run: ([$tags.Tags[]? | select(.Key == "kythira:run-id") | .Value] | first // "?")}]' \
            <<<"${out}") || return 1
    done < <(jq -r '.[] | map(tostring) | @tsv' <<<"${cas}")
    printf '%s\n' "${out}"
}

# The inventory's CAs older than the grace period: a CA created seconds ago
# may belong to a run that is still going.
stale() {
    jq --argjson now "$(date +%s)" --argjson grace "${GRACE}" \
        '[.[] | select(($now - .created) > $grace)]'
}

audit() {
    local inv leaks n now
    if ! inv=$(inventory); then
        echo "::error::THE ACM PRIVATE CA AUDIT DID NOT RUN (a list call failed). This is not a clean result: CAs tagged ${TAG_KEY}=${TAG_VALUE} may have outlived teardown and be billing now. If this is an authorization error, the CI role is missing the acm-pca bundle; re-run scripts/ci-cloud-credentials/aws/provision-oidc-role.sh." >&2
        exit 1
    fi
    leaks=$(stale <<<"${inv}")
    n=$(jq length <<<"${leaks}")
    if [ "${n}" -eq 0 ]; then
        echo "acm-pca suite: clean (no non-DELETED CA tagged ${TAG_KEY}=${TAG_VALUE} older than ${GRACE}s)."
        return 0
    fi
    now=$(date +%s)
    echo "::error::Post-run audit found ${n} ACM Private CA(s) tagged ${TAG_KEY}=${TAG_VALUE} that are not DELETED and are older than ${GRACE}s. Teardown did not complete, and each bills hourly." >&2
    jq -r --argjson now "${now}" \
        '.[] | "  \(.arn): \(.status), \((($now - .created) / 60) | floor) min old, run \(.run)"' \
        <<<"${leaks}" >&2
    exit 1
}

sweep() {
    local inv n arn status left
    if ! inv=$(inventory); then
        echo "::error::The ACM Private CA sweep could not list what to delete (a list call failed)." >&2
        exit 1
    fi
    inv=$(stale <<<"${inv}")
    n=$(jq length <<<"${inv}")
    if [ "${n}" -eq 0 ]; then
        echo "acm-pca sweep: nothing tagged ${TAG_KEY}=${TAG_VALUE} older than ${GRACE}s; nothing to do."
        return 0
    fi
    echo "acm-pca sweep: removing ${n} CA(s)."
    while IFS=$'\t' read -r arn status; do
        if [ "${status}" = "ACTIVE" ]; then
            echo "sweep: update-certificate-authority ${arn} --status DISABLED"
            aws acm-pca update-certificate-authority --certificate-authority-arn "${arn}" \
                --status DISABLED || echo "::warning::sweep: disabling ${arn} failed" >&2
        fi
        echo "sweep: delete-certificate-authority ${arn} (${status})"
        aws acm-pca delete-certificate-authority --certificate-authority-arn "${arn}" \
            --permanent-deletion-time-in-days 7 \
            || echo "::warning::sweep: deleting ${arn} failed" >&2
    done < <(jq -r '.[] | [.arn, .status] | @tsv' <<<"${inv}")

    if ! inv=$(inventory); then
        echo "::error::The ACM Private CA sweep could not re-list after deleting, so it cannot say whether it worked." >&2
        exit 1
    fi
    inv=$(stale <<<"${inv}")
    left=$(jq length <<<"${inv}")
    if [ "${left}" -ne 0 ]; then
        echo "::error::The ACM Private CA sweep left ${left} CA(s) tagged ${TAG_KEY}=${TAG_VALUE}:" >&2
        jq -r '.[] | "  \(.arn): \(.status)"' <<<"${inv}" >&2
        exit 1
    fi
    echo "acm-pca sweep: every stale CA tagged ${TAG_KEY}=${TAG_VALUE} is DELETED."
}

case "${1:-}" in
    audit) audit ;;
    sweep) sweep ;;
    *)
        echo "usage: $(basename "$0") audit|sweep" >&2
        exit 2
        ;;
esac
