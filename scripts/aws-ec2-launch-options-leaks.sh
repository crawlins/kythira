#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Leak audit and sweep for aws_ec2_launch_options_real_test.
#
# Usage: aws-ec2-launch-options-leaks.sh audit|sweep
#
#   audit  Lists everything the suite could have left behind: instances,
#          placement groups, VPCs, subnets and security groups. AWS deletes
#          asynchronously, so a finding is re-checked for up to
#          KYTHIRA_EC2LO_AUDIT_WAIT_SECONDS (default 300) and only reported
#          once it has outlived that wait. Exits 1 on a leak, and also when
#          it could not look, because an audit that cannot see is not an
#          audit that found nothing.
#
#   sweep  Deletes the same inventory: instances first (a placement group
#          or subnet with a member cannot be deleted), then placement groups,
#          then security groups, subnets and VPCs. Exits 1 if anything
#          survives. Run it AFTER the audit, so detection stays loud and
#          remediation does not hide it.
#
# Both modes find resources by the kythira:suite=aws-ec2-launch-options tag
# the test applies at creation (to instances through the manager's
# extra_tags). Nothing else in the account carries that tag, so this never
# touches resources another suite or an operator created. It does touch a
# concurrent run of the same suite, which is why the workflow job that calls
# this is serialised with a concurrency group.
#
# This is aws-asg-leaks.sh's shape for a suite with no Auto Scaling groups or
# launch templates but with placement groups.
#
# Region comes from AWS_DEFAULT_REGION / AWS_REGION, credentials from the
# usual AWS CLI chain (AWS_PROFILE works for a local run).

set -euo pipefail

TAG_KEY="kythira:suite"
TAG_VALUE="aws-ec2-launch-options"
AUDIT_WAIT="${KYTHIRA_EC2LO_AUDIT_WAIT_SECONDS:-300}"
TAG_FILTER="Name=tag:${TAG_KEY},Values=${TAG_VALUE}"

# Prints one JSON object holding every list below, or fails if any Describe
# call does. Never prints a partial inventory.
inventory() {
    local instances pgs vpcs subnets sgs
    # shellcheck disable=SC2016  # JMESPath literal backticks, not shell
    instances=$(aws ec2 describe-instances --filters "${TAG_FILTER}" \
        'Name=instance-state-name,Values=pending,running,shutting-down,stopping,stopped' \
        --query 'Reservations[].Instances[].{id:InstanceId,state:State.Name,type:InstanceType,lifecycle:InstanceLifecycle,launched:LaunchTime,cluster:Tags[?Key==`kythira:cluster`]|[0].Value}' \
        --output json) || return 1
    # A deleted placement group stays listed in state "deleted" for a while;
    # it holds nothing and bills nothing.
    # shellcheck disable=SC2016  # JMESPath literal backticks, not shell
    pgs=$(aws ec2 describe-placement-groups --filters "${TAG_FILTER}" \
        --query 'PlacementGroups[?State!=`deleted`].{name:GroupName,strategy:Strategy,state:State}' \
        --output json) || return 1
    vpcs=$(aws ec2 describe-vpcs --filters "${TAG_FILTER}" \
        --query 'Vpcs[].{id:VpcId,cidr:CidrBlock}' --output json) || return 1
    subnets=$(aws ec2 describe-subnets --filters "${TAG_FILTER}" \
        --query 'Subnets[].{id:SubnetId,vpc:VpcId}' --output json) || return 1
    sgs=$(aws ec2 describe-security-groups --filters "${TAG_FILTER}" \
        --query 'SecurityGroups[].{id:GroupId,vpc:VpcId}' --output json) || return 1
    jq -n --argjson i "${instances}" --argjson p "${pgs}" --argjson v "${vpcs}" \
        --argjson s "${subnets}" --argjson sg "${sgs}" \
        '{instances: ($i // []), placement_groups: ($p // []), vpcs: ($v // []),
          subnets: ($s // []), security_groups: ($sg // [])}'
}

count() { jq '[.[] | length] | add' <<<"$1"; }

audit() {
    local inv n deadline
    deadline=$((SECONDS + AUDIT_WAIT))
    while :; do
        if ! inv=$(inventory); then
            echo "::error::THE EC2 LAUNCH-OPTIONS AUDIT DID NOT RUN (a Describe call failed). This is not a clean result: instances or placement groups tagged ${TAG_KEY}=${TAG_VALUE} may have outlived teardown. If this is an authorization error, the CI role is missing part of the ec2-quorum-manager bundle; re-run scripts/ci-cloud-credentials/aws/provision-oidc-role.sh." >&2
            exit 1
        fi
        n=$(count "${inv}")
        if [ "${n}" -eq 0 ]; then
            echo "ec2 launch-options suite: clean (nothing tagged ${TAG_KEY}=${TAG_VALUE})."
            return 0
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            break
        fi
        echo "ec2 launch-options suite: ${n} resource(s) still present; AWS deletes asynchronously, re-checking in 30s."
        sleep 30
    done

    echo "::error::Post-run audit found ${n} resource(s) tagged ${TAG_KEY}=${TAG_VALUE} that outlived a ${AUDIT_WAIT}s wait. Teardown did not complete." >&2
    jq -r '(.instances[] | "  instance \(.id): \(.state) \(.type) \(.lifecycle // "on-demand") cluster=\(.cluster)"),
           (.placement_groups[] | "  placement group \(.name): \(.strategy) \(.state)"),
           (.security_groups[] | "  security group \(.id) in \(.vpc)"),
           (.subnets[] | "  subnet \(.id) in \(.vpc)"),
           (.vpcs[] | "  vpc \(.id)")' <<<"${inv}" >&2
    jq . <<<"${inv}" >&2
    exit 1
}

# Polls `$1` (a command printing a number) until it prints 0 or `$2` seconds
# pass. Logs either way; never fails the script on its own, so one stuck
# resource does not stop the sweep from reaching the rest.
wait_for_zero() {
    local what="$1" probe="$2" budget="$3" deadline left
    deadline=$((SECONDS + budget))
    while :; do
        left=$(eval "${probe}") || left="?"
        if [ "${left}" = "0" ]; then
            echo "sweep: ${what}: done."
            return 0
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            echo "::warning::sweep: ${what}: ${left} still present after ${budget}s; carrying on." >&2
            return 0
        fi
        sleep 15
    done
}

sweep() {
    local inv n id vpc
    if ! inv=$(inventory); then
        echo "::error::The EC2 launch-options sweep could not list what to delete (a Describe call failed)." >&2
        exit 1
    fi
    n=$(count "${inv}")
    if [ "${n}" -eq 0 ]; then
        echo "ec2 launch-options sweep: nothing tagged ${TAG_KEY}=${TAG_VALUE}; nothing to do."
        return 0
    fi
    echo "ec2 launch-options sweep: removing ${n} resource(s)."

    local ids
    ids=$(jq -r '.instances[].id' <<<"${inv}" | tr '\n' ' ')
    if [ -n "${ids// /}" ]; then
        echo "sweep: terminate-instances ${ids}"
        # shellcheck disable=SC2086  # word splitting is the point
        aws ec2 terminate-instances --instance-ids ${ids} >/dev/null \
            || echo "::warning::sweep: terminate-instances failed" >&2
        wait_for_zero "instances terminated" "inventory | jq '.instances | length'" 300
    fi

    # A placement group refuses deletion while any instance, even a
    # shutting-down one, is still a member, so retry until the budget ends.
    local deadline=$((SECONDS + 300)) pg
    while :; do
        pg=$(inventory) || pg='{"placement_groups":[]}'
        if [ "$(jq '.placement_groups | length' <<<"${pg}")" -eq 0 ]; then
            echo "sweep: placement groups deleted."
            break
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            echo "::warning::sweep: placement groups still present after 300s." >&2
            break
        fi
        for id in $(jq -r '.placement_groups[].name' <<<"${pg}"); do
            aws ec2 delete-placement-group --group-name "${id}" >/dev/null 2>&1 \
                && echo "sweep: placement group ${id} deleted"
        done
        sleep 15
    done

    # Network, innermost first, re-listed on every pass: a terminated
    # instance's network interface releases asynchronously and blocks its
    # security group and subnet until it does. Security groups are also
    # listed per VPC, so one that lost its tag still cannot pin the VPC.
    deadline=$((SECONDS + 540))
    local net
    while :; do
        net=$(inventory) || net='{"vpcs":[],"subnets":[],"security_groups":[]}'
        if [ "$(jq '(.vpcs + .subnets + .security_groups) | length' <<<"${net}")" -eq 0 ]; then
            echo "sweep: network deleted."
            break
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            echo "::warning::sweep: network still present after 540s." >&2
            break
        fi
        for id in $(jq -r '.security_groups[].id' <<<"${net}"); do
            aws ec2 delete-security-group --group-id "${id}" >/dev/null 2>&1 \
                && echo "sweep: security group ${id} deleted"
        done
        for vpc in $(jq -r '.vpcs[].id' <<<"${net}"); do
            # shellcheck disable=SC2016  # JMESPath literal backticks, not shell
            for id in $(aws ec2 describe-security-groups --filters "Name=vpc-id,Values=${vpc}" \
                --query 'SecurityGroups[?GroupName!=`default`].GroupId' --output text 2>/dev/null); do
                aws ec2 delete-security-group --group-id "${id}" >/dev/null 2>&1 \
                    && echo "sweep: security group ${id} (in ${vpc}) deleted"
            done
        done
        for id in $(jq -r '.subnets[].id' <<<"${net}"); do
            aws ec2 delete-subnet --subnet-id "${id}" >/dev/null 2>&1 \
                && echo "sweep: subnet ${id} deleted"
        done
        for id in $(jq -r '.vpcs[].id' <<<"${net}"); do
            aws ec2 delete-vpc --vpc-id "${id}" >/dev/null 2>&1 \
                && echo "sweep: vpc ${id} deleted"
        done
        sleep 15
    done

    if ! inv=$(inventory); then
        echo "::error::The EC2 launch-options sweep could not re-list after deleting, so it cannot say whether it worked." >&2
        exit 1
    fi
    n=$(count "${inv}")
    if [ "${n}" -ne 0 ]; then
        echo "::error::The EC2 launch-options sweep left ${n} resource(s) tagged ${TAG_KEY}=${TAG_VALUE}:" >&2
        jq . <<<"${inv}" >&2
        exit 1
    fi
    echo "ec2 launch-options sweep: everything tagged ${TAG_KEY}=${TAG_VALUE} is gone."
}

case "${1:-}" in
    audit) audit ;;
    sweep) sweep ;;
    *)
        echo "usage: $(basename "$0") audit|sweep" >&2
        exit 2
        ;;
esac
