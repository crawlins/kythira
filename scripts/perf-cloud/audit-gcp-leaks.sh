#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Post-run leak audit for the GCP half of the cloud performance workflow
# (.kiro/specs/multi-raft-performance/ Requirement 18.10, 18.12).
#
# The sibling of audit-aws-leaks.sh, and deliberately a re-implementation
# rather than a shared abstraction: the two clouds disagree about what a
# resource even is (GCE has no key pairs, its boot disks are zonal children
# of the instance, its firewall rules cannot carry labels). What IS shared
# is the two properties that script learned the hard way, and both are
# reproduced here rather than re-derived:
#
#   1. A query that FAILS must report UNKNOWN, never clean. An auditor that
#      cannot see is not an auditor that found nothing, and swallowing
#      stderr turns "I could not look" into "there is nothing there" —
#      which reports success precisely when it has lost the ability to
#      detect anything.
#   2. `out=$("$@" ...) || rc=$?` and not a bare assignment. Under `set -e`
#      an assignment takes the command's status, so a failing query aborts
#      the script before any of the handling below can run.
#
# GCP needs a third property AWS did not: `gcloud compute ... list` exits 0
# when authentication fails, so the exit status alone does not tell you
# whether the query saw anything. See the comment on `audit()`.
#
# Everything this harness creates carries one run-scoped LABEL, applied at
# creation time by `gcloud compute instances create --labels`, so the audit
# is a label query rather than a list of names kept in sync with the
# provisioning code.
#
# NOTE ON FIREWALL RULES. There are none to audit, and that is a design
# decision rather than an omission: the default network already carries
# `default-allow-ssh` permitting tcp:22 from 0.0.0.0/0, so a narrower
# run-scoped rule cannot subtract from it and would only add a resource
# capable of leaking. GCE firewall rules also cannot carry labels, so such
# a rule would have to be audited by name prefix — a weaker contract than
# the one every other resource here has. See run-gcp-shape-1.sh.
#
# Usage:  audit-gcp-leaks.sh <run-tag-value> [project]

set -euo pipefail

RUN_TAG="${1:?usage: audit-gcp-leaks.sh <run-tag-value> [project]}"
PROJECT="${2:-${CLOUDSDK_CORE_PROJECT:-}}"
LABEL_KEY="kythira-perf-run"

if [ -z "$PROJECT" ]; then
    echo "::error::No project given and CLOUDSDK_CORE_PROJECT is unset." >&2
    exit 1
fi

leaked=0
failed_queries=0

# `gcloud compute ... list` EXITS 0 WHEN AUTHENTICATION FAILS.
#
# It prints "WARNING: Some requests did not succeed." plus the provider's
# reason to stderr, emits nothing on stdout, and returns success. Measured
# directly: with a deliberately invalid CLOUDSDK_AUTH_ACCESS_TOKEN, four of
# this script's five queries "succeeded" with empty output and only the
# global one (snapshots) exited non-zero.
#
# That is exactly the defect audit-aws-leaks.sh was rewritten to close — an
# auditor that cannot see reporting clean — arriving through a different
# door. There it was a swallowed stderr; here it is a command that reports
# success while having looked at nothing. An exit-status check alone is not
# enough on this provider, so stderr is inspected for the partial-failure
# and authentication markers as well.
#
# stdout and stderr are captured SEPARATELY, and that is load-bearing too.
# With `2>&1` a benign warning line — gcloud emits "The following filter
# keys were not present in any resource" whenever the label matches nothing,
# which is the normal clean case — lands in the output being counted and is
# indistinguishable from a surviving resource. Merging the streams would
# make a clean run report leaks.
#
# $1 = human label, $2... = the gcloud command and its arguments
audit() {
    local label="$1"; shift
    local out err rc=0
    local errfile
    errfile=$(mktemp)
    out=$("$@" 2>"$errfile") || rc=$?
    err=$(cat "$errfile"); rm -f "$errfile"

    if [ "$rc" -ne 0 ]; then
        echo "::error::Audit query for $label FAILED (exit $rc) — this run's leak status is UNKNOWN, not clean:" >&2
        printf '%s\n' "$err" >&2
        failed_queries=$((failed_queries + 1))
        return
    fi
    # The exit-0-on-failure case. Matched on gcloud's own partial-failure
    # banner and on the authentication and permission phrasings the API
    # returns underneath it, because any of them means this query saw an
    # incomplete view of the project.
    if printf '%s' "$err" | LC_ALL=C grep -qiE 'some requests did not succeed|invalid authentication|permission_denied|insufficient (authentication scopes|permission)|not authorized'; then
        echo "::error::Audit query for $label returned exit 0 but reported a failure — leak status UNKNOWN, not clean:" >&2
        printf '%s\n' "$err" >&2
        failed_queries=$((failed_queries + 1))
        return
    fi

    # STDOUT only. `gcloud --format=value` prints nothing at all for an empty
    # result, so the count is the number of non-blank lines. `grep -c` would
    # exit 1 on zero matches and take the script down with it under `set -e`.
    local n
    n=$(printf '%s' "$out" | sed '/^[[:space:]]*$/d' | wc -l)
    if [ "$n" -ne 0 ]; then
        echo "::error::Post-run audit found $n leaked $label for run '$RUN_TAG':" >&2
        printf '%s\n' "$out" >&2
        leaked=$((leaked + n))
    else
        echo "  clean: no $label"
    fi
}

echo "Auditing project $PROJECT for resources labelled $LABEL_KEY=$RUN_TAG"

# ── Instances ────────────────────────────────────────────────────────────────
# Every state is a leak except absence. Unlike EC2 there is no `terminated`
# tombstone to exclude: a deleted GCE instance stops being listed at all, so
# anything this returns is real and is billing (a TERMINATED — i.e. stopped —
# instance still bills its boot disk).
audit "instance(s)" gcloud compute instances list \
    --project "$PROJECT" \
    --filter "labels.${LABEL_KEY}=${RUN_TAG}" \
    --format "value(name,zone,status)"

# ── Disks ────────────────────────────────────────────────────────────────────
# Audited separately rather than trusted to auto-delete. `--boot-disk-auto-
# delete` is the default for instances created with an image, but a disk
# created separately, or one whose instance creation failed after the disk
# landed, outlives it and bills indefinitely.
audit "disk(s)" gcloud compute disks list \
    --project "$PROJECT" \
    --filter "labels.${LABEL_KEY}=${RUN_TAG}" \
    --format "value(name,zone,sizeGb,type)"

# ── Reserved addresses ───────────────────────────────────────────────────────
# Shape 1 uses an ephemeral external IP, which is released with the instance
# and cannot leak. This is here for the shapes that do not: GCP bills a
# reserved address that is not attached to anything, exactly as EC2 bills an
# unassociated Elastic IP, so it is the expensive case rather than the
# harmless one.
audit "reserved address(es)" gcloud compute addresses list \
    --project "$PROJECT" \
    --filter "labels.${LABEL_KEY}=${RUN_TAG}" \
    --format "value(name,region,address,status)"

# ── Snapshots and images ─────────────────────────────────────────────────────
# Nothing in Shape 1 creates either. They are audited anyway because both
# bill by stored byte, both survive every other teardown, and a future shape
# that starts creating them should not also have to remember to extend this.
audit "snapshot(s)" gcloud compute snapshots list \
    --project "$PROJECT" \
    --filter "labels.${LABEL_KEY}=${RUN_TAG}" \
    --format "value(name,diskSizeGb,status)"

audit "image(s)" gcloud compute images list \
    --project "$PROJECT" --no-standard-images \
    --filter "labels.${LABEL_KEY}=${RUN_TAG}" \
    --format "value(name,status)"

if [ "$failed_queries" -ne 0 ]; then
    echo "::error::Post-run audit could not complete: $failed_queries of its queries failed." >&2
    echo "  The run's leak status is UNKNOWN. Treat it as leaking until checked by hand." >&2
    echo "  The usual cause is a missing IAM role on the identity running this — the CI" >&2
    echo "  service account needs roles/compute.instanceAdmin.v1 at minimum. See" >&2
    echo "  scripts/ci-cloud-credentials/gcp/README.md." >&2
    exit 1
fi

if [ "$leaked" -ne 0 ]; then
    echo "::error::Post-run audit FAILED: $leaked resource(s) survived teardown for run '$RUN_TAG'." >&2
    echo "  These are billing now. Delete them, then find out why teardown missed them." >&2
    echo "  gcloud compute instances list --project $PROJECT --filter labels.${LABEL_KEY}=${RUN_TAG}" >&2
    exit 1
fi

echo "Post-run audit clean: nothing labelled $LABEL_KEY=$RUN_TAG survives in $PROJECT."
