#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Shape 1 of the cloud performance measurement: one non-burstable AWS
# instance, Tier B, running the same benchmark binary as the local rows
# (.kiro/specs/multi-raft-performance/ Requirement 18.6, 18.7, 18.13).
#
# THE BINARY IS SHIPPED, NOT REBUILT, AND THAT IS THE EXPERIMENT.
# Requirement 18.7 wants the local-vs-cloud delta to *be* the hardware
# confound. Rebuilding on the instance would fold a different compiler
# version, a different vcpkg resolution and a different set of
# `-march`-adjacent decisions into that delta, and there would be no way
# afterwards to say which of them moved the number. `multi_raft_http_
# benchmark_test` links only libstdc++, libm, libgcc_s and libc, so the
# same Release ELF that produced the local table runs unmodified on an
# Ubuntu 24.04 AMI. The delta is then hardware and nothing else.
#
# It is also the only version of this that fits a cost ceiling. A from-
# source build of this project's vcpkg dependency set is hours, and
# Requirement 18.11's ceiling is 45 minutes for the measured phase.
#
# This script exists as a script, rather than as steps in perf-cloud.yml,
# so that the thing which spends money can be dry-run, read, and executed
# by hand from a workstation with the same code path CI takes. A
# provisioning sequence that only ever runs inside a workflow is one whose
# failure modes are only ever discovered inside a workflow.
#
# Usage:
#   scripts/perf-cloud/run-aws-shape-1.sh --binary PATH [OPTIONS]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

BINARY=""
BUILD_REF=""
ROOT_VOLUME_GB=""
INSTANCE_TYPE="c5.2xlarge"
REGION="${AWS_REGION:-us-east-1}"
RUN_TAG=""
FILTER="multi_raft_http_benchmark/write_throughput_by_rpc_serializer"
CEILING_MINUTES="45"
OUT_DIR="perf-cloud-results"
SSH_CIDR=""
REPEAT=1
ARCH=""
DRY_RUN=0
KEEP=0

TAG_KEY="kythira-perf-run"

usage() {
    cat <<'EOF'
Usage: run-aws-shape-1.sh --binary PATH [OPTIONS]

Provisions one non-burstable EC2 instance, ships a prebuilt benchmark
binary to it, runs the measured phase under a hard ceiling, pulls the
artifacts back, and tears everything down unconditionally.

Exactly one of:
  --binary PATH             Prebuilt multi_raft_http_benchmark_test. Must
                             match the instance's architecture. This is the
                             preferred mode — see the header for why.
  --build-ref SHA_OR_REF    Build the binary ON the instance from this git
                             ref of the public repository. Use this only
                             when no build host of the instance's
                             architecture is available; it costs an hour or
                             more of instance time and it puts a second
                             compiler into the local-vs-cloud delta, which
                             the artifact records so a reader can see it.

Optional:
  --instance-type TYPE      default: c5.2xlarge (burstable types refused)
  --root-volume-gb N        Root volume size. Defaults to the AMI's own (8 GiB)
                             for --binary, and 120 for --build-ref, where the
                             vcpkg build tree alone is tens of gigabytes.
  --region REGION           default: $AWS_REGION or us-east-1
  --run-tag VALUE           default: perf-local-<epoch>-<pid>. Every
                             resource carries it; the audit keys off it.
  --filter FILTER           Boost.Test --run_test filter for the measured
                             phase. default: the RPC-serializer row.
  --ceiling-minutes N       Hard ceiling on the measured phase. default: 45
  --repeat N                Run the measured phase N times on the SAME
                             instance. default: 1. Two runs of one instance
                             cost barely more than one and are the only way
                             to tell a stable ratio from a lucky one; two
                             instances would confound replication with
                             placement.
  --out-dir DIR             Where artifacts land. default: perf-cloud-results
  --ssh-cidr CIDR           Source CIDR allowed to reach port 22. Default is
                             this machine's public address as api.ipify.org
                             reports it, with a /32.
  --keep                    Skip teardown. FOR DEBUGGING ONLY; the audit
                             will fail and the instance will keep billing.
  --dry-run                 Print what would run without provisioning
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) BINARY="$2"; shift 2 ;;
        --build-ref) BUILD_REF="$2"; shift 2 ;;
        --root-volume-gb) ROOT_VOLUME_GB="$2"; shift 2 ;;
        --instance-type) INSTANCE_TYPE="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --run-tag) RUN_TAG="$2"; shift 2 ;;
        --filter) FILTER="$2"; shift 2 ;;
        --ceiling-minutes) CEILING_MINUTES="$2"; shift 2 ;;
        --repeat) REPEAT="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --ssh-cidr) SSH_CIDR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${BINARY}" && -z "${BUILD_REF}" ]]; then
    echo "ERROR: one of --binary or --build-ref is required." >&2; usage >&2; exit 1
fi
if [[ -n "${BINARY}" && -n "${BUILD_REF}" ]]; then
    echo "ERROR: --binary and --build-ref are mutually exclusive." >&2; exit 1
fi
if [[ -n "${BINARY}" && ! -f "${BINARY}" ]]; then
    echo "ERROR: no such file: ${BINARY}" >&2; exit 1
fi

# Requirement 18.5 is enforced on the instance by capture-provenance.sh,
# which is the authority. This is an earlier, cheaper copy of the same
# refusal: refusing before RunInstances costs zero cents, refusing after
# it costs one boot.
case "${INSTANCE_TYPE}" in
    t1.*|t2.*|t3.*|t3a.*|t4g.*)
        echo "ERROR: '${INSTANCE_TYPE}' is burstable. A benchmark that exhausts CPU" \
             "credits measures the credit balance, not the machine (Requirement 18.5)." >&2
        exit 1 ;;
esac

# The binary's architecture and the instance's have to agree, and getting
# this wrong is otherwise a five-minute boot followed by "cannot execute
# binary file". `file` names what the ELF actually is; the instance type's
# family names what will run it.
case "${INSTANCE_TYPE}" in
    *g.*|*gd.*|*gn.*|*gen.*) ARCH="arm64"; WANT_ARCH="ARM aarch64" ;;
    *)                       ARCH="amd64"; WANT_ARCH="x86-64" ;;
esac
if [[ -n "${BINARY}" ]]; then
    BIN_ARCH=$(LC_ALL=C file -b "${BINARY}" | sed -n 's/.*ELF 64-bit LSB[^,]*, \([^,]*\),.*/\1/p')
    if [[ "${BIN_ARCH}" != "${WANT_ARCH}" ]]; then
        echo "ERROR: ${BINARY} is '${BIN_ARCH}' but ${INSTANCE_TYPE} needs '${WANT_ARCH}'." >&2
        exit 1
    fi
else
    BIN_ARCH="built on the instance (${WANT_ARCH})"
fi
if [[ -z "${ROOT_VOLUME_GB}" && -n "${BUILD_REF}" ]]; then
    ROOT_VOLUME_GB=120
fi

if [[ -z "${RUN_TAG}" ]]; then
    RUN_TAG="perf-local-$(date -u +%s)-$$"
fi

echo "=== Shape 1 ==============================================="
echo "  region:        ${REGION}"
echo "  instance type: ${INSTANCE_TYPE} (${ARCH})"
echo "  run tag:       ${TAG_KEY}=${RUN_TAG}"
if [[ -n "${BINARY}" ]]; then
    echo "  binary:        ${BINARY} ($(stat -c %s "${BINARY}") bytes, ${BIN_ARCH})"
else
    echo "  binary:        BUILT ON THE INSTANCE from ${BUILD_REF}"
fi
echo "  filter:        ${FILTER}"
echo "  repeat:        ${REPEAT}"
echo "  ceiling:       ${CEILING_MINUTES} minutes (per repetition)"
echo "==========================================================="

aws() { command aws --region "${REGION}" "$@"; }

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] would resolve the Ubuntu 24.04 ${ARCH} AMI, create a key pair"
    echo "[dry-run] and security group tagged ${TAG_KEY}=${RUN_TAG}, launch one"
    echo "[dry-run] ${INSTANCE_TYPE}, ship ${BINARY}, run the measured phase, pull"
    echo "[dry-run] results into ${OUT_DIR}, then terminate and audit."
    exit 0
fi

WORK_DIR=$(mktemp -d)
KEY_NAME="kythira-${RUN_TAG}"
SG_NAME="kythira-${RUN_TAG}"
INSTANCE_ID=""
SG_ID=""
KEY_CREATED=0

# Unconditional teardown (Requirement 18.10). A trap rather than a
# trailing block, because the run most likely to have leaked is the one
# that failed on line forty — which is exactly when a trailing block does
# not execute. Ordered instance-first: a security group cannot be deleted
# while an instance still holds it.
teardown() {
    local rc=$?
    set +e
    if [[ "${KEEP}" == "1" ]]; then
        echo ""
        echo "!!! --keep given: NOT tearing down. These resources are billing now:"
        echo "!!!   instance:       ${INSTANCE_ID:-none}"
        echo "!!!   security group: ${SG_ID:-none}"
        echo "!!!   key pair:       ${KEY_NAME}"
        return $rc
    fi
    echo ""
    echo "=== Teardown ============================================="
    if [[ -n "${INSTANCE_ID}" ]]; then
        echo "  terminating ${INSTANCE_ID}"
        aws ec2 terminate-instances --instance-ids "${INSTANCE_ID}" >/dev/null 2>&1
        # Waiting is not politeness. DeleteSecurityGroup fails with
        # DependencyViolation while any ENI still references the group,
        # and an instance keeps its ENI until it is fully terminated.
        aws ec2 wait instance-terminated --instance-ids "${INSTANCE_ID}" 2>/dev/null
    fi
    if [[ -n "${SG_ID}" ]]; then
        echo "  deleting security group ${SG_ID}"
        for attempt in 1 2 3 4 5 6; do
            aws ec2 delete-security-group --group-id "${SG_ID}" 2>/dev/null && break
            sleep 10
        done
    fi
    if [[ "${KEY_CREATED}" == "1" ]]; then
        echo "  deleting key pair ${KEY_NAME}"
        aws ec2 delete-key-pair --key-name "${KEY_NAME}" >/dev/null 2>&1
    fi
    rm -rf "${WORK_DIR}"
    echo "=== Audit ================================================"
    # The claim that matters, and separate from the teardown on purpose:
    # "the teardown ran" and "nothing is left running" are different
    # claims. This one is independent of everything above it.
    bash "${REPO_ROOT}/scripts/perf-cloud/audit-aws-leaks.sh" "${RUN_TAG}" "${REGION}"
    local audit_rc=$?
    if [[ $rc -eq 0 && $audit_rc -ne 0 ]]; then rc=$audit_rc; fi
    # `exit`, not `return`. An EXIT trap that returns leaves the script's
    # status as whatever it already was, so a clean measured phase followed
    # by a FAILING audit would exit 0 — which is the one outcome this
    # script must never report, because it means something is still
    # billing and nothing said so.
    exit $rc
}
trap 'teardown' EXIT

TAGSPEC="ResourceType=%s,Tags=[{Key=${TAG_KEY},Value=${RUN_TAG}},{Key=Name,Value=kythira-perf-${RUN_TAG}}]"

echo ""
echo "[step] Resolve the Ubuntu 24.04 ${ARCH} AMI"
# Canonical's public SSM parameters, never a hardcoded AMI id: an AMI id
# is region-scoped and goes stale, and a stale one is a silent
# measurement on last year's kernel.
AMI_PARAM="/aws/service/canonical/ubuntu/server/24.04/stable/current/${ARCH}/hvm/ebs-gp3/ami-id"
AMI_ID=$(aws ssm get-parameters --names "${AMI_PARAM}" \
    --query 'Parameters[0].Value' --output text)
if [[ -z "${AMI_ID}" || "${AMI_ID}" == "None" ]]; then
    echo "ERROR: could not resolve ${AMI_PARAM}" >&2; exit 1
fi
echo "  ${AMI_ID}"

echo "[step] Read provider-stated instance facts"
# The control-plane half of Requirement 18.4. Read here, where the
# credentials are, and handed to the instance — which then needs no cloud
# permissions of its own.
STATED=$(aws ec2 describe-instance-types --instance-types "${INSTANCE_TYPE}" \
    --query 'InstanceTypes[0].[NetworkInfo.NetworkPerformance,VCpuInfo.DefaultVCpus,MemoryInfo.SizeInMiB]' \
    --output text)
STATED_NETWORK=$(cut -f1 <<<"${STATED}")
echo "  network: ${STATED_NETWORK}"

echo "[step] Determine the SSH source address"
if [[ -z "${SSH_CIDR}" ]]; then
    MY_IP=$(curl -fsS --max-time 10 https://api.ipify.org || true)
    if [[ -z "${MY_IP}" ]]; then
        echo "ERROR: could not determine this machine's public address. Pass" \
             "--ssh-cidr explicitly. The group is NOT opened to 0.0.0.0/0 as a" \
             "fallback — an unreachable instance is a cheaper failure than an" \
             "open one." >&2
        exit 1
    fi
    SSH_CIDR="${MY_IP}/32"
fi
echo "  ${SSH_CIDR}"

echo "[step] Create the ephemeral key pair"
# Tagged at creation, like everything else: a resource created without
# the run tag is invisible to the audit, which is the same as not being
# torn down.
aws ec2 create-key-pair --key-name "${KEY_NAME}" \
    --tag-specifications "$(printf "${TAGSPEC}" key-pair)" \
    --query 'KeyMaterial' --output text > "${WORK_DIR}/id_rsa"
chmod 600 "${WORK_DIR}/id_rsa"
KEY_CREATED=1
echo "  ${KEY_NAME}"

echo "[step] Create the security group"
VPC_ID=$(aws ec2 describe-vpcs --filters Name=isDefault,Values=true \
    --query 'Vpcs[0].VpcId' --output text)
SG_ID=$(aws ec2 create-security-group --group-name "${SG_NAME}" \
    --description "kythira perf-cloud shape 1, run ${RUN_TAG}" \
    --vpc-id "${VPC_ID}" \
    --tag-specifications "$(printf "${TAGSPEC}" security-group)" \
    --query 'GroupId' --output text)
aws ec2 authorize-security-group-ingress --group-id "${SG_ID}" \
    --protocol tcp --port 22 --cidr "${SSH_CIDR}" >/dev/null
echo "  ${SG_ID} in ${VPC_ID}, port 22 from ${SSH_CIDR}"

echo "[step] Launch"
# The fourth safety net, and the one task 17 named as the standing gap:
# the three ceilings there (the benchmark's own `timeout`, the job's
# `timeout-minutes`, the teardown) all live on the *controlling* machine.
# If this process dies, none of them fire. `shutdown -h` scheduled by
# cloud-init lives on the instance, and with
# instance-initiated-shutdown-behavior=terminate it terminates rather
# than stopping — a stopped instance still bills its volume. The margin
# over the ceiling is generous on purpose: this must never be the thing
# that ends a healthy run.
# It has to cover EVERY repetition plus, in --build-ref mode, the build —
# not one repetition. Sizing it at `ceiling + 25` was the first version and
# it is a live hazard in exactly the mode that needs the switch most: an
# on-instance build is hours, and a dead-man switch that fires mid-build
# terminates the instance, destroys the work, and looks like a network
# failure. The margin is deliberately generous; this must never be the
# thing that ends a healthy run, only the thing that ends an abandoned one.
BUILD_BUDGET_MINUTES=0
if [[ -n "${BUILD_REF}" ]]; then BUILD_BUDGET_MINUTES=300; fi
DEADMAN_MINUTES=$(( CEILING_MINUTES * REPEAT + BUILD_BUDGET_MINUTES + 30 ))
echo "  dead-man shutdown scheduled at +${DEADMAN_MINUTES} minutes from boot"
USER_DATA=$(cat <<EOF
#!/bin/bash
shutdown -h +${DEADMAN_MINUTES} "kythira perf-cloud dead-man switch"
EOF
)
BDM=()
if [[ -n "${ROOT_VOLUME_GB}" ]]; then
    # DeleteOnTermination is set explicitly rather than left to the AMI's
    # default. It happens to be true for Canonical's images, but a leaked
    # 120 GiB gp3 bills about nine dollars a month forever and "the AMI
    # probably does the right thing" is not a control.
    BDM=(--block-device-mappings
         "DeviceName=/dev/sda1,Ebs={VolumeSize=${ROOT_VOLUME_GB},VolumeType=gp3,DeleteOnTermination=true}")
fi
INSTANCE_ID=$(aws ec2 run-instances \
    --image-id "${AMI_ID}" \
    "${BDM[@]}" \
    --instance-type "${INSTANCE_TYPE}" \
    --key-name "${KEY_NAME}" \
    --security-group-ids "${SG_ID}" \
    --instance-initiated-shutdown-behavior terminate \
    --metadata-options "HttpTokens=required,HttpEndpoint=enabled" \
    --user-data "${USER_DATA}" \
    --tag-specifications "$(printf "${TAGSPEC}" instance)" \
                         "$(printf "${TAGSPEC}" volume)" \
    --count 1 \
    --query 'Instances[0].InstanceId' --output text)
echo "  ${INSTANCE_ID}"

echo "[step] Wait for the instance to run"
aws ec2 wait instance-running --instance-ids "${INSTANCE_ID}"
PUBLIC_IP=$(aws ec2 describe-instances --instance-ids "${INSTANCE_ID}" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
AZ=$(aws ec2 describe-instances --instance-ids "${INSTANCE_ID}" \
    --query 'Reservations[0].Instances[0].Placement.AvailabilityZone' --output text)
TENANCY=$(aws ec2 describe-instances --instance-ids "${INSTANCE_ID}" \
    --query 'Reservations[0].Instances[0].Placement.Tenancy' --output text)
echo "  ${PUBLIC_IP} in ${AZ}, tenancy ${TENANCY}"

VOLUME=$(aws ec2 describe-volumes \
    --filters "Name=attachment.instance-id,Values=${INSTANCE_ID}" \
    --query 'Volumes[0].[VolumeType,Iops]' --output text)
STATED_STORAGE=$(cut -f1 <<<"${VOLUME}")
STATED_IOPS=$(cut -f2 <<<"${VOLUME}")
echo "  root volume ${STATED_STORAGE}, ${STATED_IOPS} IOPS"

SSH_OPTS=(-i "${WORK_DIR}/id_rsa" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=10 -o ServerAliveInterval=30)

echo "[step] Wait for SSH"
# `instance-running` means the hypervisor started it, not that sshd is
# listening — there is a minute or two of cloud-init between the two, and
# scp-ing into that window fails in a way that looks like a network
# problem.
for attempt in $(seq 1 60); do
    if ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" true 2>/dev/null; then
        echo "  up after $((attempt * 5))s"
        break
    fi
    if [[ "${attempt}" == "60" ]]; then
        echo "ERROR: sshd never answered on ${PUBLIC_IP} within 300s." >&2
        echo "--- console output ---" >&2
        aws ec2 get-console-output --instance-id "${INSTANCE_ID}" \
            --query 'Output' --output text 2>/dev/null | tail -40 >&2
        exit 1
    fi
    sleep 5
done

echo "[step] Ship the provenance script"
scp "${SSH_OPTS[@]}" -q "${REPO_ROOT}/scripts/perf-cloud/capture-provenance.sh" \
    "ubuntu@${PUBLIC_IP}:/tmp/"
ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "chmod +x /tmp/capture-provenance.sh"

BIN_NAME="multi_raft_http_benchmark_test"
if [[ -n "${BINARY}" ]]; then
    echo "[step] Ship the binary"
    BIN_NAME=$(basename "${BINARY}")
    scp "${SSH_OPTS[@]}" -q "${BINARY}" "ubuntu@${PUBLIC_IP}:/tmp/"
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "chmod +x /tmp/${BIN_NAME}"
    BINARY_SHA=$(sha256sum "${BINARY}" | cut -d' ' -f1)
    BINARY_BYTES=$(stat -c %s "${BINARY}")
    BINARY_ORIGIN="shipped from the controlling machine"
else
    echo "[step] Build on the instance from ${BUILD_REF}"
    # The build recipe is ci.yml's, deliberately: same compiler (g++-13),
    # same Release build type, same configs/ci_full_defconfig, same pinned
    # vcpkg commit. A cloud row built against a different feature set is
    # measuring a different program, and the whole point of Requirement
    # 18.7 is that only the hardware differs.
    #
    # It is still one more difference than --binary has, and the artifact
    # says so: `binary_origin` in run.json distinguishes the two.
    scp "${SSH_OPTS[@]}" -q "${REPO_ROOT}/scripts/perf-cloud/build-benchmark-on-instance.sh" \
        "ubuntu@${PUBLIC_IP}:/tmp/"
    # Started DETACHED as a systemd unit and polled through a state
    # artifact, not held open on one ssh channel. This build is hours; a
    # single long-lived channel across a NAT is a coin flip, and losing it
    # would abandon a running instance with no controlling process — which
    # is precisely the leak the dead-man switch exists for and which should
    # not be routine.
    #
    # systemd-run rather than `setsid ... &`, and that was learned the hard
    # way: a backgrounded remote command still holds the ssh session
    # channel open, so `ssh host "cmd &"` does not return until `cmd`
    # finishes no matter how its own streams are redirected. The first
    # version of this hung for the whole build with no progress output and
    # no way to poll. systemd-run hands the process to pid 1 and returns.
    #
    # /tmp/build.rc is the state artifact the loop below waits on, rather
    # than the unit's own status: "the file the build writes when it is
    # done" survives a systemd restart, a reconnect, and a poll that misses
    # the window in which the unit was still active.
    mkdir -p "${OUT_DIR}"
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
        "chmod +x /tmp/build-benchmark-on-instance.sh && rm -f /tmp/build.rc /tmp/build.log && \
         sudo systemd-run --unit=kythira-perf-build --collect \
             --property=User=ubuntu \
             --property=WorkingDirectory=/home/ubuntu \
             --property=Environment=HOME=/home/ubuntu \
             --property=StandardOutput=append:/tmp/build.log \
             --property=StandardError=append:/tmp/build.log \
             /bin/bash -c '/tmp/build-benchmark-on-instance.sh ${BUILD_REF}; echo \$? > /tmp/build.rc'"
    BUILD_RC="unknown"
    BUILD_DEADLINE=$(( $(date +%s) + BUILD_BUDGET_MINUTES * 60 ))
    while true; do
        if BUILD_RC=$(ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "cat /tmp/build.rc 2>/dev/null" 2>/dev/null) \
           && [[ -n "${BUILD_RC}" ]]; then
            break
        fi
        if [[ $(date +%s) -ge ${BUILD_DEADLINE} ]]; then
            echo "ERROR: the on-instance build did not finish within ${BUILD_BUDGET_MINUTES} minutes." >&2
            BUILD_RC="timeout"
            break
        fi
        # Progress, so a four-hour step is not a four-hour silence.
        ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
            "tail -n 1 /tmp/build.log 2>/dev/null" 2>/dev/null | sed 's/^/    build: /' || true
        sleep 120
    done
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/build.log" "${OUT_DIR}/" || true
    if [[ "${BUILD_RC}" != "0" ]]; then
        echo "ERROR: the on-instance build exited ${BUILD_RC}. See ${OUT_DIR}/build.log." >&2
        tail -40 "${OUT_DIR}/build.log" >&2 || true
        exit 1
    fi
    BINARY_SHA=$(ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "sha256sum /tmp/${BIN_NAME} | cut -d' ' -f1")
    BINARY_BYTES=$(ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "stat -c %s /tmp/${BIN_NAME}")
    BINARY_ORIGIN="built on the instance from ${BUILD_REF}"
    echo "  built ${BIN_NAME} (${BINARY_BYTES} bytes)"
fi

echo "[step] Capture provenance (Requirement 18.4)"
# Runs before the measured phase, and its burstable refusal is a hard
# failure: a row that should never be published is cheaper to prevent
# than to explain.
# Every one of these is KYTHIRA_PERF_STATED_*, and the prefix is not
# decoration: capture-provenance.sh reads exactly these names and emits
# null for anything it does not find. The first live run passed three of
# them as KYTHIRA_PERF_TENANCY / _STATED_STORAGE / _STATED_IOPS and got
# three nulls in the artifact — which is the design working (18.4's "null,
# never guessed" made the mismatch visible) and is also why they are
# listed here against the script's own variable list rather than from
# memory.
#
# _PLACEMENT is the placement GROUP, not the availability zone — the zone
# is already a separate field that IMDS fills in. Shape 1 uses no
# placement group, so it is deliberately empty and lands as null;
# Requirement 18.8's Shape 2 is where it becomes a real value.
PLACEMENT_GROUP=""
ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
    "KYTHIRA_PERF_STATED_NETWORK='${STATED_NETWORK}' \
     KYTHIRA_PERF_STATED_STORAGE_CLASS='${STATED_STORAGE}' \
     KYTHIRA_PERF_STATED_STORAGE_IOPS='${STATED_IOPS}' \
     KYTHIRA_PERF_STATED_TENANCY='${TENANCY}' \
     KYTHIRA_PERF_STATED_PLACEMENT='${PLACEMENT_GROUP}' \
     KYTHIRA_PERF_STATED_INSTANCE_TYPE='${INSTANCE_TYPE}' \
     /tmp/capture-provenance.sh /tmp/provenance.json"

mkdir -p "${OUT_DIR}"
scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/provenance.json" "${OUT_DIR}/" || true

RCS=()
for rep in $(seq 1 "${REPEAT}"); do
    echo "[step] Measured phase ${rep}/${REPEAT} (ceiling ${CEILING_MINUTES} minutes)"
    # `timeout --signal=KILL` and not a plain timeout: the point of a
    # ceiling is that it holds even when the process under it is wedged in
    # a way that ignores TERM. This is the first of the three ceilings; it
    # bounds work, and only the teardown above bounds billing.
    set +e
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
        "cd /tmp && timeout --signal=KILL ${CEILING_MINUTES}m ./${BIN_NAME} \
           --run_test='${FILTER}' --log_level=test_suite > /tmp/run${rep}.log 2>&1; \
         echo \$? > /tmp/run${rep}.rc"
    BENCH_SSH_RC=$?
    set -e
    if [[ ${BENCH_SSH_RC} -ne 0 ]]; then
        echo "  WARNING: the ssh carrying repetition ${rep} exited ${BENCH_SSH_RC}." >&2
    fi

    # Pulled per repetition, before the next one starts and before any
    # judgement about success. A failed run's log is the only thing that
    # explains it, and the instance is about to be destroyed.
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
        "sed -e 's/\x1b\[[0-9;]*m//g' /tmp/run${rep}.log > /tmp/run${rep}.clean.log" || true
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/run${rep}.clean.log" "${OUT_DIR}/" || true
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/run${rep}.rc" "${OUT_DIR}/" || true
    rc=$(cat "${OUT_DIR}/run${rep}.rc" 2>/dev/null || echo "unknown")
    RCS+=("${rc}")
    echo "  repetition ${rep} exited ${rc}"
    # Freeing the log keeps a long --repeat off the root volume; these are
    # tens of megabytes each and the instance has 8 GiB.
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "rm -f /tmp/run${rep}.log" || true
done

BENCH_RC="${RCS[0]}"
for rc in "${RCS[@]}"; do
    if [[ "${rc}" != "0" ]]; then BENCH_RC="${rc}"; fi
done
echo "  artifacts in ${OUT_DIR}/ (exit codes: ${RCS[*]})"

# The run metadata the artifact needs and the instance cannot know: what
# was asked for, and of which build.
python3 - "${OUT_DIR}/run.json" <<PY
import json, sys, os
json.dump({
    "shape": "aws-shape-1",
    "run_tag": "${RUN_TAG}",
    "region": "${REGION}",
    "availability_zone": "${AZ}",
    "instance_type": "${INSTANCE_TYPE}",
    "instance_id": "${INSTANCE_ID}",
    "image_id": "${AMI_ID}",
    "arch": "${ARCH}",
    "benchmark_filter": "${FILTER}",
    "ceiling_minutes": ${CEILING_MINUTES},
    "repetitions": ${REPEAT},
    "benchmark_exit_codes": "${RCS[*]}",
    "binary_sha256": "${BINARY_SHA}",
    "binary_bytes": ${BINARY_BYTES},
    "binary_origin": "${BINARY_ORIGIN}",
    "git_commit": "$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)",
}, open(sys.argv[1], "w"), indent=2)
PY
echo "  ${OUT_DIR}/run.json"

if [[ "${BENCH_RC}" != "0" ]]; then
    echo "ERROR: a measured phase exited ${BENCH_RC} (all: ${RCS[*]}). See ${OUT_DIR}/run*.clean.log." >&2
    exit 1
fi
echo ""
echo "Measured phase completed. Teardown and audit follow."
