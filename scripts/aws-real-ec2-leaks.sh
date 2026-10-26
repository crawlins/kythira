#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Sweep for networks and instances the real-EC2 suites leaked.
#
# Usage: aws-real-ec2-leaks.sh sweep [--dry-run]
#
# The aws job's "Audit for leaked instances" step detects leaks; this removes
# them, and runs only after that audit has failed so the job stays red.
# Detection stays loud and remediation does not hide it, the same split as
# aws-acm-pca-leaks.sh.
#
# What it touches: only VPCs stamped kythira:github-run-id (the real-EC2
# fixtures stamp every VPC at CreateVpc, see tag_vpc_with_github_run() in
# tests/aws_real_ec2_test_support.hpp) whose run the Actions API reports as
# completed. A completed run's tests are over, so anything it still owns is a
# leak and nothing live can depend on it. Everything else the audit reports
# is left alone and listed:
#
#   - a VPC stamped with a run that is still going (the audit already skips
#     these) or whose status cannot be read;
#   - an unstamped VPC, which could be an operator's local run of the same
#     suites (no GITHUB_RUN_ID, so no stamp) that is still in progress;
#   - an instance outside a swept VPC.
#
# Those still need a person, and the audit keeps failing until they are gone.
#
# Run 39962794527 is why this exists: its chaos-crash case hung on an SSH
# read until ctest killed it, the fixture's teardown never ran, and three
# t3.micro plus their VPC billed for three days because the audit reported
# them on every later run and nothing removed them.
#
# Order inside each VPC follows the fixtures' own teardown: instances
# (waited until terminated), NAT gateways (waited until deleted, then their
# Elastic IPs released, since the fixtures do not tag the EIP), then key
# pair, security groups, subnets, route tables, network ACLs, internet
# gateways, placement groups and the VPC. The network deletes are retried in
# passes, because a terminated instance's network interface detaches
# asynchronously and EC2 refuses DeleteSecurityGroup and DeleteSubnet until
# it has.
#
# The CI role cannot DescribeRouteTables, so route tables, key pairs and
# placement groups are found by the Name prefix of their VPC
# (kyt-<uuid>-vpc -> kyt-<uuid>-*), the names every fixture gives them.
# TerminateInstances is granted only on instances tagged kythira:managed-by,
# so an untagged instance in a leaked VPC is reported, not removed.
#
# Exits 1 if anything it set out to remove survives. Needs aws, jq and gh
# (GH_TOKEN in CI; a logged-in gh works locally, with AWS_PROFILE and
# AWS_DEFAULT_REGION set). --dry-run prints what it would do.

# shellcheck disable=SC2016  # JMESPath literal backticks throughout, not shell
set -euo pipefail

RUN_TAG="kythira:github-run-id"
REPO="${GITHUB_REPOSITORY:-crawlins/kythira}"
DRY_RUN=0
PASSES="${KYTHIRA_EC2_SWEEP_PASSES:-12}"
PASS_SLEEP="${KYTHIRA_EC2_SWEEP_PASS_SLEEP:-15}"

# Runs a mutating call, or prints it under --dry-run. A *NotFound error
# counts as done: something else, often an earlier pass, already removed it.
act() {
    local out
    if [ "${DRY_RUN}" -eq 1 ]; then
        echo "  dry-run: aws $*"
        return 0
    fi
    if out=$(aws "$@" 2>&1); then
        echo "  ok: aws $*"
        return 0
    fi
    case "${out}" in
        *NotFound*) echo "  gone: aws $*"; return 0 ;;
    esac
    echo "  failed: aws $*: ${out}" >&2
    return 1
}

# Prints one line per VPC this sweep may remove: "<vpc-id>\t<name>\t<run>".
# Fails if the VPC listing fails. A run whose status cannot be read is
# skipped with a warning, never assumed finished.
sweepable_vpcs() {
    local stamped run status
    stamped=$(aws ec2 describe-vpcs \
        --filters "Name=tag-key,Values=${RUN_TAG}" \
        --query 'Vpcs[].{id:VpcId,name:Tags[?Key==`Name`]|[0].Value,run:Tags[?Key==`kythira:github-run-id`]|[0].Value}' \
        --output json) || return 1
    for run in $(jq -r '[.[].run] | unique | .[]' <<<"${stamped}"); do
        if [ "${run}" = "${GITHUB_RUN_ID:-}" ]; then
            status=completed
        elif ! status=$(gh api "repos/${REPO}/actions/runs/${run}" --jq .status 2>/dev/null); then
            echo "::warning::sweep: could not read the status of run ${run}; leaving its VPCs for a person." >&2
            continue
        fi
        if [ "${status}" != "completed" ]; then
            echo "sweep: run ${run} is ${status}; leaving its VPCs alone." >&2
            continue
        fi
        jq -r --arg r "${run}" '.[] | select(.run == $r) | [.id, (.name // ""), .run] | @tsv' <<<"${stamped}"
    done
}

# Name-tag lookup for resources the CI role cannot describe directly.
ids_by_name_prefix() {
    local type="$1" prefix="$2"
    aws ec2 describe-tags \
        --filters "Name=resource-type,Values=${type}" "Name=key,Values=Name" "Name=value,Values=${prefix}-*" \
        --query 'Tags[].ResourceId' --output text
}

terminate_instances() {
    local vid="$1" all ids untagged
    all=$(aws ec2 describe-instances \
        --filters "Name=vpc-id,Values=${vid}" 'Name=instance-state-name,Values=pending,running,stopping,stopped' \
        --query 'Reservations[].Instances[].{id:InstanceId,managed:Tags[?Key==`kythira:managed-by`]|[0].Value}' \
        --output json) || return 1
    untagged=$(jq -r '[.[] | select(.managed == null) | .id] | join(" ")' <<<"${all}")
    if [ -n "${untagged}" ]; then
        echo "::error::sweep: ${vid} holds instance(s) without kythira:managed-by (${untagged}); the CI role cannot terminate them, so they and their VPC need a person." >&2
        return 2
    fi
    ids=$(jq -r '[.[] | select(.managed != null) | .id] | join(" ")' <<<"${all}")
    [ -n "${ids}" ] || return 0
    # shellcheck disable=SC2086  # ids is a space-separated list
    act ec2 terminate-instances --instance-ids ${ids} || return 1
    [ "${DRY_RUN}" -eq 1 ] && return 0
    # shellcheck disable=SC2086
    aws ec2 wait instance-terminated --instance-ids ${ids}
}

delete_nat_gateways() {
    local vid="$1" nats id allocs i state
    nats=$(aws ec2 describe-nat-gateways \
        --filter "Name=vpc-id,Values=${vid}" 'Name=state,Values=pending,available,failed' \
        --query 'NatGateways[].{id:NatGatewayId,allocs:NatGatewayAddresses[].AllocationId}' \
        --output json) || return 1
    for id in $(jq -r '.[].id' <<<"${nats}"); do
        act ec2 delete-nat-gateway --nat-gateway-id "${id}" || return 1
        [ "${DRY_RUN}" -eq 1 ] && continue
        for i in $(seq 1 40); do
            state=$(aws ec2 describe-nat-gateways --nat-gateway-ids "${id}" \
                --query 'NatGateways[0].State' --output text 2>/dev/null) || state=deleted
            [ "${state}" = "deleted" ] && break
            [ "${i}" -eq 40 ] && { echo "  NAT ${id} still ${state} after 10 min" >&2; return 1; }
            sleep 15
        done
    done
    allocs=$(jq -r '[.[].allocs[]?] | join(" ")' <<<"${nats}")
    for id in ${allocs}; do
        act ec2 release-address --allocation-id "${id}" || return 1
    done
}

# One pass over the VPC's network. Returns 0 once the VPC itself is gone.
network_pass() {
    local vid="$1" prefix="$2" failed=0 id
    if [ -n "${prefix}" ]; then
        for id in $(aws ec2 describe-key-pairs --filters "Name=key-name,Values=${prefix}-*" \
                --query 'KeyPairs[].KeyName' --output text); do
            act ec2 delete-key-pair --key-name "${id}" || failed=1
        done
    fi
    for id in $(aws ec2 describe-security-groups --filters "Name=vpc-id,Values=${vid}" \
            --query 'SecurityGroups[?GroupName!=`default`].GroupId' --output text); do
        act ec2 delete-security-group --group-id "${id}" || failed=1
    done
    for id in $(aws ec2 describe-subnets --filters "Name=vpc-id,Values=${vid}" \
            --query 'Subnets[].SubnetId' --output text); do
        act ec2 delete-subnet --subnet-id "${id}" || failed=1
    done
    if [ -n "${prefix}" ]; then
        for id in $(ids_by_name_prefix route-table "${prefix}"); do
            act ec2 delete-route-table --route-table-id "${id}" || failed=1
        done
    fi
    for id in $(aws ec2 describe-network-acls --filters "Name=vpc-id,Values=${vid}" \
            --query 'NetworkAcls[?IsDefault==`false`].NetworkAclId' --output text); do
        act ec2 delete-network-acl --network-acl-id "${id}" || failed=1
    done
    for id in $(aws ec2 describe-internet-gateways --filters "Name=attachment.vpc-id,Values=${vid}" \
            --query 'InternetGateways[].InternetGatewayId' --output text); do
        act ec2 detach-internet-gateway --internet-gateway-id "${id}" --vpc-id "${vid}" || failed=1
        act ec2 delete-internet-gateway --internet-gateway-id "${id}" || failed=1
    done
    if [ -n "${prefix}" ]; then
        for id in $(aws ec2 describe-placement-groups --filters "Name=group-name,Values=${prefix}-*" \
                --query 'PlacementGroups[].GroupName' --output text); do
            act ec2 delete-placement-group --group-name "${id}" || failed=1
        done
    fi
    act ec2 delete-vpc --vpc-id "${vid}" || failed=1
    return "${failed}"
}

sweep_vpc() {
    local vid="$1" name="$2" run="$3" prefix="" pass
    case "${name}" in kyt-*-vpc) prefix="${name%-vpc}" ;; esac
    echo "sweep: ${vid} (${name:-unnamed}) from run ${run}: ${GITHUB_SERVER_URL:-https://github.com}/${REPO}/actions/runs/${run}"
    terminate_instances "${vid}" || {
        # 2: an instance this role cannot terminate, which keeps the VPC
        # alive whatever the network passes do.
        [ $? -eq 2 ] && return 1
        echo "::warning::sweep: terminating instances in ${vid} did not finish cleanly." >&2
    }
    delete_nat_gateways "${vid}" || echo "::warning::sweep: NAT gateway cleanup in ${vid} did not finish cleanly." >&2
    for pass in $(seq 1 "${PASSES}"); do
        if network_pass "${vid}" "${prefix}"; then
            return 0
        fi
        [ "${DRY_RUN}" -eq 1 ] && return 0
        [ "${pass}" -lt "${PASSES}" ] && sleep "${PASS_SLEEP}"
    done
    return 1
}

sweep() {
    local list vid name run failed=0
    if ! list=$(sweepable_vpcs); then
        echo "::error::The real-EC2 sweep could not list run-stamped VPCs, so it removed nothing." >&2
        exit 1
    fi
    if [ -z "${list}" ]; then
        echo "real-ec2 sweep: no VPC stamped by a completed run; nothing to remove. Whatever the audit reported needs a person."
        exit 0
    fi
    while IFS=$'\t' read -r vid name run; do
        sweep_vpc "${vid}" "${name}" "${run}" || {
            echo "::error::sweep: ${vid} is still there; the messages above say what holds it." >&2
            failed=1
        }
    done <<<"${list}"
    [ "${failed}" -eq 0 ] || exit 1
    echo "real-ec2 sweep: every VPC stamped by a completed run is gone."
}

case "${1:-}" in
    sweep)
        [ "${2:-}" = "--dry-run" ] && DRY_RUN=1
        sweep
        ;;
    *)
        echo "usage: $(basename "$0") sweep [--dry-run]" >&2
        exit 2
        ;;
esac
