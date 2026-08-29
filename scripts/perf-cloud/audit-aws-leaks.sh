#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Post-run leak audit for the cloud performance workflow
# (.kiro/specs/multi-raft-performance/ Requirement 18.10).
#
# "The teardown ran" and "nothing is left running" are different claims, and
# this project has had every cloud provider leak something on its first live
# run. This asserts the second claim, independently of the teardown, and it
# FAILS the job when anything survives -- a warning would be read once and then
# never again, and the resource keeps billing either way.
#
# Everything the workflow creates is tagged with a single run-scoped tag, so
# the audit is a tag query rather than a list of resource names to keep in
# sync with the provisioning code. The tag is the contract: a resource created
# without it is invisible here, which is why the workflow tags at creation
# time (RunInstances --tag-specifications) rather than in a later CreateTags
# call that a crash could skip.
#
# Usage:  audit-aws-leaks.sh <run-tag-value> [region]

set -euo pipefail

RUN_TAG="${1:?usage: audit-aws-leaks.sh <run-tag-value> [region]}"
REGION="${2:-${AWS_REGION:-us-east-1}}"
TAG_KEY="kythira-perf-run"

leaked=0
failed_queries=0

# Runs one describe call and reports it. The error handling here is the whole
# point of the function and is deliberately not `2>/dev/null`:
#
# A leak auditor that cannot see is not a leak auditor that found nothing. If
# the query itself fails -- an AccessDenied because the role is missing an
# action, a throttle, a bad region -- then swallowing stderr turns "I could not
# look" into an empty result, which prints "clean" and exits 0. That is the
# worst possible failure for this script: it reports success precisely when it
# has lost the ability to detect anything. So a failed query is counted
# separately and fails the job on its own, with the provider's own message.
#
# $1 = human label, $2... = the aws command and its arguments
audit() {
    local label="$1"; shift
    local out rc=0
    # `|| rc=$?` rather than a bare assignment followed by `rc=$?`: under
    # `set -e` the assignment takes the command's exit status, so a failing
    # describe would abort the script right here and none of the handling
    # below would run -- which is how this was first written, and it exited
    # 254 with no diagnosis instead of reporting UNKNOWN. The `||` makes the
    # assignment a tested command, which `set -e` leaves alone.
    out=$("$@" 2>&1) || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "::error::Audit query for $label FAILED (exit $rc) — this run's leak status is UNKNOWN, not clean:" >&2
        printf '%s\n' "$out" >&2
        failed_queries=$((failed_queries + 1))
        return
    fi
    local n
    n=$(printf '%s' "$out" | jq 'if . == null then 0 else length end' 2>/dev/null || echo "")
    if [ -z "$n" ]; then
        echo "::error::Audit query for $label returned output jq could not read — leak status UNKNOWN:" >&2
        printf '%s\n' "$out" >&2
        failed_queries=$((failed_queries + 1))
        return
    fi
    if [ "$n" -ne 0 ]; then
        echo "::error::Post-run audit found $n leaked $label for run '$RUN_TAG':" >&2
        printf '%s' "$out" | jq . >&2
        leaked=$((leaked + n))
    else
        echo "  clean: no $label"
    fi
}

echo "Auditing region $REGION for resources tagged $TAG_KEY=$RUN_TAG"

# ── Instances ────────────────────────────────────────────────────────────────
# `terminated` and `shutting-down` are excluded: a terminated instance is a
# record, not a resource, and it stops billing. Anything else -- including
# `stopped`, which still bills its EBS volume -- counts as a leak.
audit "instance(s)" aws ec2 describe-instances --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'Reservations[].Instances[?State.Name!=`terminated` && State.Name!=`shutting-down`][].{id:InstanceId,state:State.Name,type:InstanceType,launched:LaunchTime}' \
    --output json

# ── Volumes ──────────────────────────────────────────────────────────────────
# Audited separately from instances rather than trusted to
# DeleteOnTermination: that attribute defaults to false for any volume
# attached after launch, and an orphaned gp3 bills indefinitely and silently.
audit "volume(s)" aws ec2 describe-volumes --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'Volumes[].{id:VolumeId,state:State,size:Size,type:VolumeType}' \
    --output json

# ── Elastic IPs ──────────────────────────────────────────────────────────────
# An *unassociated* address is the expensive case -- AWS bills idle addresses
# precisely to discourage hoarding -- so a leak here is worse than one that is
# still attached to something.
audit "elastic IP(s)" aws ec2 describe-addresses --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'Addresses[].{ip:PublicIp,alloc:AllocationId,assoc:AssociationId}' \
    --output json

# ── Security groups ──────────────────────────────────────────────────────────
audit "security group(s)" aws ec2 describe-security-groups --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'SecurityGroups[].{id:GroupId,name:GroupName}' \
    --output json

# ── Placement groups ─────────────────────────────────────────────────────────
# Requirement 18.8's Tier E shape uses one; it costs nothing by itself, but a
# leaked placement group holds a name and will collide with the next run,
# which turns a free leak into a confusing failure later.
audit "placement group(s)" aws ec2 describe-placement-groups --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'PlacementGroups[].{name:GroupName,state:State,strategy:Strategy}' \
    --output json

# ── Key pairs ────────────────────────────────────────────────────────────────
audit "key pair(s)" aws ec2 describe-key-pairs --region "$REGION" \
    --filters "Name=tag:$TAG_KEY,Values=$RUN_TAG" \
    --query 'KeyPairs[].{name:KeyName,id:KeyPairId}' \
    --output json

if [ "$failed_queries" -ne 0 ]; then
    echo "::error::Post-run audit could not complete: $failed_queries of its queries failed." >&2
    echo "  The run's leak status is UNKNOWN. Treat it as leaking until checked by hand." >&2
    echo "  The usual cause is a missing IAM action on the CI role — see" >&2
    echo "  scripts/ci-cloud-credentials/aws/policies/perf-cloud.json and re-run" >&2
    echo "  provision-oidc-role.sh with the perf-cloud bundle." >&2
    exit 1
fi

if [ "$leaked" -ne 0 ]; then
    echo "::error::Post-run audit FAILED: $leaked resource(s) survived teardown for run '$RUN_TAG'." >&2
    echo "  These are billing now. Delete them, then find out why teardown missed them." >&2
    echo "  aws ec2 describe-instances --region $REGION --filters Name=tag:$TAG_KEY,Values=$RUN_TAG" >&2
    exit 1
fi

echo "Post-run audit clean: nothing tagged $TAG_KEY=$RUN_TAG survives in $REGION."
