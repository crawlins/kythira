#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Shape 2 of the cloud performance measurement: N `multi_raft_node` hosts on N
# EC2 instances and `multi_raft_bench` on one of its own. Tier E, on real
# machines. `.kiro/specs/multi-machine-placement/` tasks 2, 3 and 4.
#
# WHAT THIS EXISTS TO INTRODUCE IS THE NETWORK.
# Every inter-node figure this project has published is a loopback figure. The
# Tier E rows `.kiro/specs/multi-raft-host-binary/` produced were three
# rootless-Podman containers on one machine, and their inter-node probe read
# 0.7-0.9 ms -- which is what loopback gives, because containers on one host
# share a network stack. This script puts the hosts on separate machines so
# that the round trip is a real one, and records it beside the row.
#
# It is Shape 1's structure widened, and deliberately so: the run-scoped tag,
# the EXIT-trap teardown, the instance-local dead-man switch, the shipped
# binary and the provenance capture all generalise unchanged. Three things do
# not, and they are where the new failure modes live:
#
#   1. N+1 instances rather than one, so every ceiling and every audit has to
#      be sized for the set rather than the instance.
#   2. A security group rule Shape 1 never needed -- the nodes must reach each
#      other. Shape 1 allows SSH from the controller and nothing else, so
#      without this the cluster comes up and never elects.
#   3. Placement is an axis. One AZ, several AZs, or a cluster placement
#      group, chosen at run time and recorded verbatim on the row.
#
# EXPECT THE NUMBER TO BE WORSE THAN TIER C, AND THAT IS THE POINT
# (Requirement 4.5 and the requirements document's "explicitly out of scope:
# making the number good"). A Tier E row that resembled the Tier C row would
# mean the network was not real.
#
# Usage:
#   scripts/perf-cloud/run-aws-shape-2.sh --node-binary PATH --bench-binary PATH [OPTIONS]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

NODE_BINARY=""
BENCH_BINARY=""
NODES=3
INSTANCE_TYPE="c5.2xlarge"
DRIVER_INSTANCE_TYPE=""
REGION="${AWS_REGION:-us-east-1}"
RUN_TAG=""
PLACEMENT="single-az"
GROUP_COUNT=4
OPERATIONS=400
IN_FLIGHT=16
VALUE_BYTES=128
REPETITIONS=5
SCENARIO="write"
TICK_INTERVAL=2
TRANSPORT="httplib"
PERSISTENCE="memory"
DATA_THREADS=0
KEY_SPACE=100000
CEILING_MINUTES=45
OUT_DIR="perf-cloud-results-shape2"
SSH_CIDR=""
DISCOVERY="static"
ALSO_TIER_C=0
DRY_RUN=0
KEEP=0

TAG_KEY="kythira-perf-run"

# One machine each, so the ports are constants rather than a stride. Shape 1's
# Tier C launcher packs three ports per host onto one machine and computes
# them; here every host has its own address and the arithmetic would only
# obscure which port is which.
RAFT_PORT=9001
DATA_PORT=9101
CONTROL_PORT=9201

usage() {
    cat <<'EOF'
Usage: run-aws-shape-2.sh --node-binary PATH --bench-binary PATH [OPTIONS]

Provisions N+1 EC2 instances -- N running multi_raft_node, one running
multi_raft_bench -- ships prebuilt binaries to them, measures the network
between every ordered pair BEFORE the window, takes a Tier E row, pulls
every host's log back, and tears everything down unconditionally.

Required:
  --node-binary PATH        Prebuilt multi_raft_node, matching the
                             instance architecture.
  --bench-binary PATH       Prebuilt multi_raft_bench, likewise.

Both are SHIPPED, never rebuilt on the instance (doctrine 134): a rebuild
folds a different compiler and a different dependency resolution into the
delta the tier exists to isolate.

Cluster and placement:
  --nodes N                 Host instances (default 3). One more is
                             launched for the driver.
  --instance-type TYPE      Host instance type (default c5.2xlarge).
                             Burstable types are refused.
  --driver-instance-type T  Driver instance type. Defaults to
                             --instance-type. Recorded separately on the
                             row: a driver that cannot offer the load makes
                             the cluster look fast (Requirement 1.2).
  --placement MODE          single-az | multi-az | cluster (default
                             single-az).
                               single-az  the floor: real NICs, real
                                          switches, no cross-AZ hop
                               multi-az   what a fault-tolerant deployment
                                          actually pays
                               cluster    a cluster placement group: the
                                          tightest network AWS sells
  --region REGION           default: $AWS_REGION or us-east-1
  --run-tag VALUE           default: perf-local-<epoch>-<pid>

Workload -- the same axes a Tier C row takes, so that the only difference
between the two rows is where the processes are (Requirement 1.4):
  --groups N                (default 4)
  --operations N            (default 400)
  --in-flight N             (default 16)
  --value-bytes N           (default 128)
  --repetitions N           (default 5; fewer yields no headline at all)
  --scenario NAME           write | read-state | read-log | read-local
  --tick-interval MS        (default 2)
  --transport NAME          httplib | beast (default httplib)
  --persistence MODE        memory | file-buffered | file-barrier (memory)
  --data-threads N          Threads serving each host's data path (0 =
                             cpp-httplib's default).
  --key-space N             (default 100000)

Safety and output:
  --ceiling-minutes N       Hard ceiling on the measured phase (default 45).
  --discovery MODE          static | ec2-tag (default static). The static
                             list is the default for a measured row on
                             purpose: discovery inside a window is a cost in
                             that window (Requirement 3.4).
  --also-tier-c             After the Tier E row, take a Tier C row ON THE
                             DRIVER INSTANCE -- every host process and the
                             driver co-located on one machine. This is task
                             6's deliverable: the same binaries, the same
                             workload and the SAME INSTANCE TYPE, differing in
                             placement alone. A Tier C row taken on a
                             development machine instead would differ in the
                             hardware too, and the delta would say nothing
                             about placement. Costs a couple of minutes of
                             instance time already paid for.
  --out-dir DIR             default: perf-cloud-results-shape2
  --ssh-cidr CIDR           Source CIDR allowed to reach port 22. Default is
                             this machine's public address with a /32.
  --keep                    Skip teardown. FOR DEBUGGING ONLY; N+1 instances
                             keep billing and the audit will fail.
  --dry-run                 Print what would run without provisioning.
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node-binary) NODE_BINARY="$2"; shift 2 ;;
        --bench-binary) BENCH_BINARY="$2"; shift 2 ;;
        --nodes) NODES="$2"; shift 2 ;;
        --instance-type) INSTANCE_TYPE="$2"; shift 2 ;;
        --driver-instance-type) DRIVER_INSTANCE_TYPE="$2"; shift 2 ;;
        --placement) PLACEMENT="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --run-tag) RUN_TAG="$2"; shift 2 ;;
        --groups) GROUP_COUNT="$2"; shift 2 ;;
        --operations) OPERATIONS="$2"; shift 2 ;;
        --in-flight) IN_FLIGHT="$2"; shift 2 ;;
        --value-bytes) VALUE_BYTES="$2"; shift 2 ;;
        --repetitions) REPETITIONS="$2"; shift 2 ;;
        --scenario) SCENARIO="$2"; shift 2 ;;
        --tick-interval) TICK_INTERVAL="$2"; shift 2 ;;
        --transport) TRANSPORT="$2"; shift 2 ;;
        --persistence) PERSISTENCE="$2"; shift 2 ;;
        --data-threads) DATA_THREADS="$2"; shift 2 ;;
        --key-space) KEY_SPACE="$2"; shift 2 ;;
        --ceiling-minutes) CEILING_MINUTES="$2"; shift 2 ;;
        --discovery) DISCOVERY="$2"; shift 2 ;;
        --also-tier-c) ALSO_TIER_C=1; shift ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --ssh-cidr) SSH_CIDR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# ── Refusals, all of them before anything is provisioned ─────────────────────
if [[ -z "${NODE_BINARY}" || -z "${BENCH_BINARY}" ]]; then
    echo "ERROR: both --node-binary and --bench-binary are required." >&2
    usage >&2; exit 1
fi
for b in "${NODE_BINARY}" "${BENCH_BINARY}"; do
    if [[ ! -f "${b}" ]]; then echo "ERROR: no such file: ${b}" >&2; exit 1; fi
done
case "${PLACEMENT}" in
    single-az|multi-az|cluster) ;;
    *) echo "ERROR: --placement must be single-az, multi-az or cluster." >&2; exit 1 ;;
esac
case "${DISCOVERY}" in
    static|ec2-tag) ;;
    *) echo "ERROR: --discovery must be static or ec2-tag." >&2; exit 1 ;;
esac
if [[ "${NODES}" -lt 1 ]]; then
    echo "ERROR: --nodes must be at least 1." >&2; exit 1
fi
# Requirement 6.3's spread rule needs five windows before it will emit a
# headline at all, and a Tier E run is the most expensive way this project has
# to discover that it took four. Refusing here costs nothing; refusing after
# provisioning costs the whole run.
if [[ "${REPETITIONS}" -lt 5 ]]; then
    echo "ERROR: --repetitions ${REPETITIONS} cannot produce a quotable row." >&2
    echo "  The stability rule requires 5 windows and yields NO headline below" >&2
    echo "  that. An expensive run is a reason to budget enough repetitions," >&2
    echo "  never a reason to quote a bad row (Requirement 6.5)." >&2
    exit 1
fi

DRIVER_INSTANCE_TYPE="${DRIVER_INSTANCE_TYPE:-${INSTANCE_TYPE}}"

# Requirement 18.5's burstable refusal, applied to BOTH roles. The driver is
# as capable of exhausting CPU credits as a host is, and a throttled driver
# reports the cluster as slow -- a failure that looks like a finding.
for t in "${INSTANCE_TYPE}" "${DRIVER_INSTANCE_TYPE}"; do
    case "${t}" in
        t1.*|t2.*|t3.*|t3a.*|t4g.*)
            echo "ERROR: '${t}' is burstable. A benchmark that exhausts CPU credits" \
                 "measures the credit balance, not the machine (Requirement 18.5)." >&2
            exit 1 ;;
    esac
done

# A cluster placement group is a single-AZ construct by definition: its whole
# purpose is to pack instances onto one low-latency segment. Asking for one
# across zones is a contradiction AWS reports late and confusingly.
if [[ "${PLACEMENT}" == "cluster" && "${NODES}" -lt 2 ]]; then
    echo "ERROR: a cluster placement group with fewer than 2 hosts measures nothing." >&2
    exit 1
fi

case "${INSTANCE_TYPE}" in
    *g.*|*gd.*|*gn.*|*gen.*) ARCH="arm64"; WANT_ARCH="ARM aarch64" ;;
    *)                       ARCH="amd64"; WANT_ARCH="x86-64" ;;
esac
for b in "${NODE_BINARY}" "${BENCH_BINARY}"; do
    bin_arch=$(LC_ALL=C file -b "${b}" | sed -n 's/.*ELF 64-bit LSB[^,]*, \([^,]*\),.*/\1/p')
    if [[ "${bin_arch}" != "${WANT_ARCH}" ]]; then
        echo "ERROR: ${b} is '${bin_arch}' but ${INSTANCE_TYPE} needs '${WANT_ARCH}'." >&2
        exit 1
    fi
done
# The driver's architecture has to match its own instance, which is a
# different instance type and therefore possibly a different architecture.
case "${DRIVER_INSTANCE_TYPE}" in
    *g.*|*gd.*|*gn.*|*gen.*) DRIVER_WANT_ARCH="ARM aarch64"; DRIVER_ARCH="arm64" ;;
    *)                       DRIVER_WANT_ARCH="x86-64"; DRIVER_ARCH="amd64" ;;
esac
bench_arch=$(LC_ALL=C file -b "${BENCH_BINARY}" | sed -n 's/.*ELF 64-bit LSB[^,]*, \([^,]*\),.*/\1/p')
if [[ "${bench_arch}" != "${DRIVER_WANT_ARCH}" ]]; then
    echo "ERROR: ${BENCH_BINARY} is '${bench_arch}' but the driver's" \
         "${DRIVER_INSTANCE_TYPE} needs '${DRIVER_WANT_ARCH}'." >&2
    exit 1
fi

if [[ -z "${RUN_TAG}" ]]; then
    RUN_TAG="perf-local-$(date -u +%s)-$$"
fi

TOTAL_INSTANCES=$(( NODES + 1 ))

# ── The pre-registered estimate (Requirement 5.1) ────────────────────────────
# Stated BEFORE the run, as Shape 1 does. Its two AWS rows were pre-registered
# at $0.11-$0.13 and came in at $0.09 and $0.34. This is not a billing
# integration and does not pretend to be: it is an order-of-magnitude figure a
# reader can check against the invoice, and its purpose is that somebody wrote
# a number down first.
HOURLY_GUESS=$(python3 -c "
rates = {'c5.2xlarge': 0.34, 'c5.4xlarge': 0.68, 'c5.xlarge': 0.17,
         'c6i.2xlarge': 0.34, 'm5.2xlarge': 0.384, 'c5.large': 0.085}
h = rates.get('${INSTANCE_TYPE}')
d = rates.get('${DRIVER_INSTANCE_TYPE}')
print('%.2f' % (h * ${NODES} + d) if h and d else 'unknown')
" 2>/dev/null || echo unknown)

echo "=== Shape 2 — Tier E on real machines ====================="
echo "  region:          ${REGION}"
echo "  placement:       ${PLACEMENT}"
echo "  hosts:           ${NODES} x ${INSTANCE_TYPE} (${ARCH})"
echo "  driver:          1 x ${DRIVER_INSTANCE_TYPE} (${DRIVER_ARCH})"
echo "  instances:       ${TOTAL_INSTANCES} total"
echo "  run tag:         ${TAG_KEY}=${RUN_TAG}"
echo "  workload:        ${SCENARIO}, ${GROUP_COUNT} groups, ${OPERATIONS} ops,"
echo "                   ${IN_FLIGHT} in flight, ${REPETITIONS} repetitions"
echo "  transport:       ${TRANSPORT}, persistence ${PERSISTENCE}"
echo "  discovery:       ${DISCOVERY}"
echo "  ceiling:         ${CEILING_MINUTES} minutes on the measured phase"
if [[ "${HOURLY_GUESS}" != "unknown" ]]; then
    echo "  ESTIMATED COST:  ~\$${HOURLY_GUESS}/hr for ${TOTAL_INSTANCES} instances;"
    echo "                   a run that boots, probes, measures and tears down"
    echo "                   inside 30 minutes is about \$$(python3 -c "print('%.2f' % (${HOURLY_GUESS} / 2))")."
    echo "                   The number to watch is the ORPHAN: ${TOTAL_INSTANCES}"
    echo "                   instances left running is \$$(python3 -c "print('%.0f' % (${HOURLY_GUESS} * 24))")/day."
else
    echo "  ESTIMATED COST:  unknown for ${INSTANCE_TYPE}; check before running."
fi
if [[ "${PERSISTENCE}" == "memory" ]]; then
    echo "  NOTE: persistence is 'memory'. This row is comparable to an external"
    echo "        NO-FSYNC number and to nothing else (Requirement 6.2)."
fi
echo "==========================================================="

aws() { command aws --region "${REGION}" "$@"; }

if [[ "${DRY_RUN}" == "1" ]]; then
    cat <<EOF

[dry-run] Nothing is provisioned. This run would:
[dry-run]  1. resolve the Ubuntu 24.04 ${ARCH} AMI from Canonical's SSM parameter
[dry-run]  2. create key pair   kythira-${RUN_TAG}
[dry-run]     create sec group  kythira-${RUN_TAG}
[dry-run]       ingress tcp/22 from the controller's address
[dry-run]       ingress tcp/${RAFT_PORT},${DATA_PORT},${CONTROL_PORT} FROM THE GROUP ITSELF
[dry-run]       (the rule Shape 1 never needed; without it the cluster never elects)
EOF
    if [[ "${PLACEMENT}" == "cluster" ]]; then
        echo "[dry-run]     create placement group kythira-${RUN_TAG} (strategy cluster)"
        echo "[dry-run]       needs ec2:CreatePlacementGroup — see perf-cloud.json"
    fi
    cat <<EOF
[dry-run]  3. launch ${NODES} host instance(s) and 1 driver instance,
[dry-run]     placement ${PLACEMENT}, every one tagged ${TAG_KEY}=${RUN_TAG},
[dry-run]     each with instance-initiated-shutdown-behavior=terminate and a
[dry-run]     cloud-init dead-man switch sized for the WHOLE run
[dry-run]  4. wait for ssh, then capture provenance on every instance BEFORE
[dry-run]     shipping anything (doctrine 145)
[dry-run]  5. ship $(basename "${NODE_BINARY}") to ${NODES} host(s) and
[dry-run]     $(basename "${BENCH_BINARY}") to the driver — never a rebuild
[dry-run]  6. start each host under systemd-run, poll /ready (which means
[dry-run]     every group has a leader, not merely that the process is up)
[dry-run]  7. probe RTT and bandwidth between EVERY ORDERED PAIR, and measure a
[dry-run]     loopback baseline on each host for Requirement 4.5's check
[dry-run]  8. run the driver: ${REPETITIONS} repetitions of ${OPERATIONS} ${SCENARIO}
[dry-run]     operations at ${IN_FLIGHT} in flight
[dry-run]  9. pull every host's log and the driver's CSV/JSON into ${OUT_DIR}/
[dry-run] 10. terminate all ${TOTAL_INSTANCES}, delete the group(s) and key pair
[dry-run]     from an EXIT trap, then audit for leaks and FAIL if any survive

[dry-run] The host command line each node would get:
[dry-run]   multi_raft_node --node-id <i> --bind 0.0.0.0 \\
[dry-run]     --raft-port ${RAFT_PORT} --data-port ${DATA_PORT} --control-port ${CONTROL_PORT} \\
[dry-run]     --voters $(seq -s, 1 "${NODES}") --groups ${GROUP_COUNT} \\
[dry-run]     --key-count ${KEY_SPACE} --tick-interval ${TICK_INTERVAL} \\
[dry-run]     --transport ${TRANSPORT} --persistence ${PERSISTENCE} \\
[dry-run]     --data-threads ${DATA_THREADS} \\
[dry-run]     --peer <i>=http://<private-ip-i>:${RAFT_PORT} (x${NODES}) \\
[dry-run]     --peer-control <i>=<private-ip-i>:${CONTROL_PORT} (x${NODES})

[dry-run] The driver command line:
[dry-run]   multi_raft_bench --host <i>@<private-ip-i>:${DATA_PORT}:${CONTROL_PORT} (x${NODES}) \\
[dry-run]     --groups ${GROUP_COUNT} --key-space ${KEY_SPACE} --tier e \\
[dry-run]     --placement "<recorded verbatim, with every AZ>" \\
[dry-run]     --transport ${TRANSPORT} --durability ${PERSISTENCE} \\
[dry-run]     --tick-interval ${TICK_INTERVAL} --operations ${OPERATIONS} \\
[dry-run]     --in-flight ${IN_FLIGHT} --value-bytes ${VALUE_BYTES} \\
[dry-run]     --repetitions ${REPETITIONS} --scenario ${SCENARIO} \\
[dry-run]     --axis tier-e --out-dir <collected>
EOF
    exit 0
fi

WORK_DIR=$(mktemp -d)
KEY_NAME="kythira-${RUN_TAG}"
SG_NAME="kythira-${RUN_TAG}"
PG_NAME="kythira-${RUN_TAG}"
SG_ID=""
KEY_CREATED=0
PG_CREATED=0
INSTANCE_IDS=()

# ── Teardown, from an EXIT trap (Requirement 5.4) ────────────────────────────
# A trap and not a trailing block: the run most likely to leak is the one that
# failed partway, which is exactly when a trailing block does not execute.
# Shape 1 learned this from a run killed by hand. With N+1 instances the stake
# is N+1 times larger.
#
# Ordered instances-first: neither a security group nor a placement group can
# be deleted while an instance still references it.
teardown() {
    local rc=$?
    set +e
    if [[ "${KEEP}" == "1" ]]; then
        echo ""
        echo "!!! --keep given: NOT tearing down. These are billing now:"
        echo "!!!   instances:      ${INSTANCE_IDS[*]:-none}"
        echo "!!!   security group: ${SG_ID:-none}"
        echo "!!!   placement grp:  ${PG_NAME} (created: ${PG_CREATED})"
        echo "!!!   key pair:       ${KEY_NAME}"
        return $rc
    fi
    echo ""
    echo "=== Teardown ============================================="
    if [[ ${#INSTANCE_IDS[@]} -gt 0 ]]; then
        echo "  terminating ${#INSTANCE_IDS[@]} instance(s): ${INSTANCE_IDS[*]}"
        aws ec2 terminate-instances --instance-ids "${INSTANCE_IDS[@]}" >/dev/null 2>&1
        # Waiting is not politeness: DeleteSecurityGroup fails with
        # DependencyViolation while any ENI still references the group, and an
        # instance holds its ENI until it is fully terminated. `wait` polls all
        # of them together rather than serially.
        aws ec2 wait instance-terminated --instance-ids "${INSTANCE_IDS[@]}" 2>/dev/null
    fi
    if [[ -n "${SG_ID}" ]]; then
        echo "  deleting security group ${SG_ID}"
        for _ in 1 2 3 4 5 6; do
            aws ec2 delete-security-group --group-id "${SG_ID}" 2>/dev/null && break
            sleep 10
        done
    fi
    if [[ "${PG_CREATED}" == "1" ]]; then
        # A leaked placement group costs nothing and still matters: it holds
        # its name, and the next run with the same name collides. That is a
        # free leak turning into a confusing failure later.
        echo "  deleting placement group ${PG_NAME}"
        for _ in 1 2 3 4 5 6; do
            aws ec2 delete-placement-group --group-name "${PG_NAME}" 2>/dev/null && break
            sleep 10
        done
    fi
    if [[ "${KEY_CREATED}" == "1" ]]; then
        echo "  deleting key pair ${KEY_NAME}"
        aws ec2 delete-key-pair --key-name "${KEY_NAME}" >/dev/null 2>&1
    fi
    rm -rf "${WORK_DIR}"
    echo "=== Audit ================================================"
    # Independent of the teardown on purpose: "the teardown ran" and "nothing
    # is left running" are different claims and only the second one matters.
    bash "${REPO_ROOT}/scripts/perf-cloud/audit-aws-leaks.sh" "${RUN_TAG}" "${REGION}"
    local audit_rc=$?
    if [[ $rc -eq 0 && $audit_rc -ne 0 ]]; then rc=$audit_rc; fi
    # `exit`, not `return`: a clean measured phase followed by a FAILING audit
    # must not exit 0, because that is the one outcome meaning something is
    # still billing and nothing said so.
    exit $rc
}
trap 'teardown' EXIT

tagspec() { printf 'ResourceType=%s,Tags=[{Key=%s,Value=%s},{Key=Name,Value=kythira-perf-%s}]' \
    "$1" "${TAG_KEY}" "${RUN_TAG}" "${RUN_TAG}"; }

echo ""
echo "[step] Resolve the Ubuntu 24.04 ${ARCH} AMI"
AMI_PARAM="/aws/service/canonical/ubuntu/server/24.04/stable/current/${ARCH}/hvm/ebs-gp3/ami-id"
AMI_ID=$(aws ssm get-parameters --names "${AMI_PARAM}" --query 'Parameters[0].Value' --output text)
if [[ -z "${AMI_ID}" || "${AMI_ID}" == "None" ]]; then
    echo "ERROR: could not resolve ${AMI_PARAM}" >&2; exit 1
fi
echo "  ${AMI_ID}"
DRIVER_AMI_ID="${AMI_ID}"
if [[ "${DRIVER_ARCH}" != "${ARCH}" ]]; then
    DRIVER_AMI_ID=$(aws ssm get-parameters \
        --names "/aws/service/canonical/ubuntu/server/24.04/stable/current/${DRIVER_ARCH}/hvm/ebs-gp3/ami-id" \
        --query 'Parameters[0].Value' --output text)
    echo "  driver: ${DRIVER_AMI_ID} (${DRIVER_ARCH})"
fi

echo "[step] Read provider-stated instance facts"
STATED=$(aws ec2 describe-instance-types --instance-types "${INSTANCE_TYPE}" \
    --query 'InstanceTypes[0].[NetworkInfo.NetworkPerformance,VCpuInfo.DefaultVCpus,MemoryInfo.SizeInMiB]' \
    --output text)
STATED_NETWORK=$(cut -f1 <<<"${STATED}")
echo "  hosts: ${STATED_NETWORK}"

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

echo "[step] Choose subnets for placement '${PLACEMENT}'"
VPC_ID=$(aws ec2 describe-vpcs --filters Name=isDefault,Values=true \
    --query 'Vpcs[0].VpcId' --output text)
if [[ -z "${VPC_ID}" || "${VPC_ID}" == "None" ]]; then
    echo "ERROR: no default VPC in ${REGION}." >&2; exit 1
fi
# Sorted by AZ so the subnet-to-node assignment is deterministic: two runs of
# the same placement must put node 1 in the same zone, or a "same everything
# but placement" comparison silently moved something else too
# (Requirement 2.4).
mapfile -t SUBNETS < <(aws ec2 describe-subnets \
    --filters "Name=vpc-id,Values=${VPC_ID}" "Name=default-for-az,Values=true" \
    --query 'sort_by(Subnets,&AvailabilityZone)[].[SubnetId,AvailabilityZone]' \
    --output text)
if [[ ${#SUBNETS[@]} -eq 0 ]]; then
    echo "ERROR: no default subnets in ${VPC_ID}." >&2; exit 1
fi
if [[ "${PLACEMENT}" == "multi-az" && ${#SUBNETS[@]} -lt 2 ]]; then
    echo "ERROR: --placement multi-az needs at least two availability zones;" \
         "${REGION} offers ${#SUBNETS[@]}." >&2
    exit 1
fi
echo "  ${#SUBNETS[@]} zone(s) available in ${VPC_ID}"

# Which subnet each instance lands in. Index 0..NODES-1 are hosts; index NODES
# is the driver.
#
# The driver's placement is a decision worth stating: it goes in the SAME zone
# as host 1 in every mode, including multi-az. A driver in a third zone would
# add a client-side network hop to every operation and fold it into the
# cluster's number, which is the confound Requirement 1.2 exists to prevent --
# the point of giving the driver its own instance is to take the client's CPU
# out of the cluster's number, not to add a WAN to the client's path.
SUBNET_OF=()
AZ_OF=()
for (( i = 0; i < TOTAL_INSTANCES; ++i )); do
    case "${PLACEMENT}" in
        single-az|cluster) idx=0 ;;
        multi-az)
            if (( i < NODES )); then idx=$(( i % ${#SUBNETS[@]} )); else idx=0; fi ;;
    esac
    SUBNET_OF+=("$(cut -f1 <<<"${SUBNETS[$idx]}")")
    AZ_OF+=("$(cut -f2 <<<"${SUBNETS[$idx]}")")
done
echo "  hosts in: $(printf '%s ' "${AZ_OF[@]:0:${NODES}}")"
echo "  driver in: ${AZ_OF[$NODES]}"

echo "[step] Create the ephemeral key pair"
aws ec2 create-key-pair --key-name "${KEY_NAME}" \
    --tag-specifications "$(tagspec key-pair)" \
    --query 'KeyMaterial' --output text > "${WORK_DIR}/id_rsa"
chmod 600 "${WORK_DIR}/id_rsa"
KEY_CREATED=1
echo "  ${KEY_NAME}"

echo "[step] Create the security group"
SG_ID=$(aws ec2 create-security-group --group-name "${SG_NAME}" \
    --description "kythira perf-cloud shape 2, run ${RUN_TAG}" \
    --vpc-id "${VPC_ID}" \
    --tag-specifications "$(tagspec security-group)" \
    --query 'GroupId' --output text)
aws ec2 authorize-security-group-ingress --group-id "${SG_ID}" \
    --protocol tcp --port 22 --cidr "${SSH_CIDR}" >/dev/null

# THE RULE SHAPE 1 NEVER NEEDED, and the first thing that would silently
# produce a cluster that never elects. Shape 1 has one instance and allows SSH
# from the controller and nothing else; here the nodes must reach each other's
# Raft port, and the driver must reach every host's data and control port.
#
# Source is the group ITSELF rather than a CIDR. A CIDR would have to be the
# VPC's, which is every instance in the account's default VPC; the
# self-reference is exactly the N+1 instances of this run and nothing else,
# and it needs no knowledge of addresses that do not exist yet.
for port in "${RAFT_PORT}" "${DATA_PORT}" "${CONTROL_PORT}"; do
    aws ec2 authorize-security-group-ingress --group-id "${SG_ID}" \
        --protocol tcp --port "${port}" --source-group "${SG_ID}" >/dev/null
done
echo "  ${SG_ID}: tcp/22 from ${SSH_CIDR}, tcp/${RAFT_PORT},${DATA_PORT},${CONTROL_PORT} from itself"

PLACEMENT_ARG=()
if [[ "${PLACEMENT}" == "cluster" ]]; then
    echo "[step] Create the cluster placement group"
    if ! aws ec2 create-placement-group --group-name "${PG_NAME}" --strategy cluster \
        --tag-specifications "$(tagspec placement-group)" >/dev/null 2>"${WORK_DIR}/pg.err"; then
        echo "ERROR: could not create the placement group." >&2
        sed 's/^/  /' "${WORK_DIR}/pg.err" >&2
        if grep -q 'ec2:CreatePlacementGroup' "${WORK_DIR}/pg.err"; then
            echo "  This principal can DescribePlacementGroups (the leak audit's" >&2
            echo "  check) but not create one. That asymmetry is why the audit's" >&2
            echo "  placement-group class had never fired. Grant" >&2
            echo "  ec2:CreatePlacementGroup and ec2:DeletePlacementGroup — they are" >&2
            echo "  in scripts/ci-cloud-credentials/aws/policies/perf-cloud.json." >&2
        fi
        exit 1
    fi
    PG_CREATED=1
    PLACEMENT_ARG=(--placement "GroupName=${PG_NAME}")
    echo "  ${PG_NAME} (strategy cluster)"
fi

# ── The dead-man switch (Requirement 5.3) ────────────────────────────────────
# Sized for the WHOLE run, not one repetition: doctrine 135 says a dead-man
# switch sized for the fast path is not a safety net. It has to cover boot,
# provenance, deploy, readiness, the network probe, every repetition, and the
# collection afterwards. The margin is deliberately generous; this must never
# be the thing that ends a healthy run, only the thing that ends an abandoned
# one.
#
# On EVERY instance, and that is the widening that matters here. Shape 1 could
# orphan one instance; Shape 2 can orphan N+1, and the controlling process
# dying is precisely when none of the other ceilings fire.
DEADMAN_MINUTES=$(( CEILING_MINUTES * REPETITIONS + 45 ))
echo "[step] Launch ${TOTAL_INSTANCES} instances"
echo "  dead-man shutdown on each at +${DEADMAN_MINUTES} minutes from boot"
USER_DATA=$(cat <<EOF
#!/bin/bash
shutdown -h +${DEADMAN_MINUTES} "kythira perf-cloud shape 2 dead-man switch"
EOF
)

launch_one() {
    local subnet="$1" itype="$2" ami="$3" role="$4"
    aws ec2 run-instances \
        --image-id "${ami}" \
        --instance-type "${itype}" \
        --key-name "${KEY_NAME}" \
        --security-group-ids "${SG_ID}" \
        --subnet-id "${subnet}" \
        "${PLACEMENT_ARG[@]}" \
        --instance-initiated-shutdown-behavior terminate \
        --metadata-options "HttpTokens=required,HttpEndpoint=enabled" \
        --user-data "${USER_DATA}" \
        --tag-specifications \
            "ResourceType=instance,Tags=[{Key=${TAG_KEY},Value=${RUN_TAG}},{Key=Name,Value=kythira-perf-${RUN_TAG}-${role}},{Key=kythira-role,Value=${role}}]" \
            "$(tagspec volume)" \
        --count 1 \
        --query 'Instances[0].InstanceId' --output text
}

for (( i = 0; i < NODES; ++i )); do
    id=$(launch_one "${SUBNET_OF[$i]}" "${INSTANCE_TYPE}" "${AMI_ID}" "host-$(( i + 1 ))")
    INSTANCE_IDS+=("${id}")
    echo "  host $(( i + 1 )): ${id} in ${AZ_OF[$i]}"
done
DRIVER_ID=$(launch_one "${SUBNET_OF[$NODES]}" "${DRIVER_INSTANCE_TYPE}" "${DRIVER_AMI_ID}" "driver")
INSTANCE_IDS+=("${DRIVER_ID}")
echo "  driver:  ${DRIVER_ID} in ${AZ_OF[$NODES]}"

echo "[step] Wait for all ${TOTAL_INSTANCES} to run"
aws ec2 wait instance-running --instance-ids "${INSTANCE_IDS[@]}"

PUBLIC_IP=()
PRIVATE_IP=()
REAL_AZ=()
for id in "${INSTANCE_IDS[@]}"; do
    read -r pub priv az <<<"$(aws ec2 describe-instances --instance-ids "${id}" \
        --query 'Reservations[0].Instances[0].[PublicIpAddress,PrivateIpAddress,Placement.AvailabilityZone]' \
        --output text)"
    PUBLIC_IP+=("${pub}"); PRIVATE_IP+=("${priv}"); REAL_AZ+=("${az}")
done
for (( i = 0; i < TOTAL_INSTANCES; ++i )); do
    role="host $(( i + 1 ))"; [[ $i -eq NODES ]] && role="driver"
    echo "  ${role}: ${PUBLIC_IP[$i]} (private ${PRIVATE_IP[$i]}) in ${REAL_AZ[$i]}"
done

SSH_OPTS=(-i "${WORK_DIR}/id_rsa" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=10 -o ServerAliveInterval=30)
ssh_to() { local idx="$1"; shift; ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP[$idx]}" "$@"; }

echo "[step] Wait for SSH on all ${TOTAL_INSTANCES}"
for (( i = 0; i < TOTAL_INSTANCES; ++i )); do
    for attempt in $(seq 1 60); do
        if ssh_to "$i" true 2>/dev/null; then
            echo "  ${PUBLIC_IP[$i]} up after $(( attempt * 5 ))s"
            break
        fi
        if [[ "${attempt}" == "60" ]]; then
            echo "ERROR: sshd never answered on ${PUBLIC_IP[$i]} within 300s." >&2
            echo "--- console output ---" >&2
            aws ec2 get-console-output --instance-id "${INSTANCE_IDS[$i]}" \
                --query 'Output' --output text 2>/dev/null | tail -40 >&2
            exit 1
        fi
        sleep 5
    done
done

mkdir -p "${OUT_DIR}"

# BEFORE the binaries, on every instance (doctrine 145). Provenance depends on
# the instance and nothing else, and its burstable refusal is a hard failure --
# so running it first makes a refusal cost seconds rather than a whole deploy.
# The first live Graviton run failed here, on a script bug, after fifty-five
# minutes of on-instance build.
PLACEMENT_GROUP_NAME=""
if [[ "${PG_CREATED}" == "1" ]]; then
    PLACEMENT_GROUP_NAME="${PG_NAME}"
fi
echo "[step] Capture provenance on every instance (Requirement 18.4)"
for (( i = 0; i < TOTAL_INSTANCES; ++i )); do
    role="host-$(( i + 1 ))"; itype="${INSTANCE_TYPE}"
    if [[ $i -eq NODES ]]; then role="driver"; itype="${DRIVER_INSTANCE_TYPE}"; fi
    scp "${SSH_OPTS[@]}" -q "${REPO_ROOT}/scripts/perf-cloud/capture-provenance.sh" \
        "ubuntu@${PUBLIC_IP[$i]}:/tmp/"
    # _PLACEMENT is the placement GROUP, not the zone -- the zone is a
    # separate field IMDS fills in. Shape 1 always left this empty because it
    # never made one; this is where Requirement 18.8 said it becomes a real
    # value, and for --placement cluster it now is.
    ssh_to "$i" "chmod +x /tmp/capture-provenance.sh && \
        KYTHIRA_PERF_STATED_NETWORK='${STATED_NETWORK}' \
        KYTHIRA_PERF_STATED_TENANCY='default' \
        KYTHIRA_PERF_STATED_PLACEMENT='${PLACEMENT_GROUP_NAME}' \
        KYTHIRA_PERF_STATED_INSTANCE_TYPE='${itype}' \
        /tmp/capture-provenance.sh /tmp/provenance.json"
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$i]}:/tmp/provenance.json" \
        "${OUT_DIR}/provenance-${role}.json" || true
    echo "  ${role}: ${OUT_DIR}/provenance-${role}.json"
done

echo "[step] Ship the prebuilt binaries (never a rebuild — doctrine 134)"
NODE_BIN_NAME=$(basename "${NODE_BINARY}")
BENCH_BIN_NAME=$(basename "${BENCH_BINARY}")
for (( i = 0; i < NODES; ++i )); do
    scp "${SSH_OPTS[@]}" -q "${NODE_BINARY}" "ubuntu@${PUBLIC_IP[$i]}:/tmp/"
    ssh_to "$i" "chmod +x /tmp/${NODE_BIN_NAME}"
done
scp "${SSH_OPTS[@]}" -q "${BENCH_BINARY}" "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/"
ssh_to "${NODES}" "chmod +x /tmp/${BENCH_BIN_NAME}"
if [[ "${ALSO_TIER_C}" == "1" ]]; then
    # The Tier C arm runs every host process on the driver instance, so that
    # instance needs the node binary too. Shipped here, with everything else,
    # so the deploy step remains the only place binaries move.
    scp "${SSH_OPTS[@]}" -q "${NODE_BINARY}" "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/"
    scp "${SSH_OPTS[@]}" -q "${REPO_ROOT}/scripts/run-tier-c-row.sh" \
        "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/"
    ssh_to "${NODES}" "chmod +x /tmp/${NODE_BIN_NAME} /tmp/run-tier-c-row.sh"
fi
NODE_SHA=$(sha256sum "${NODE_BINARY}" | cut -d' ' -f1)
BENCH_SHA=$(sha256sum "${BENCH_BINARY}" | cut -d' ' -f1)
echo "  ${NODE_BIN_NAME} -> ${NODES} host(s); ${BENCH_BIN_NAME} -> driver"

# ── Start the hosts ──────────────────────────────────────────────────────────
PEER_ARGS=()
PEER_CONTROL_ARGS=()
HOST_ARGS=()
for (( i = 0; i < NODES; ++i )); do
    nid=$(( i + 1 ))
    PEER_ARGS+=(--peer "${nid}=http://${PRIVATE_IP[$i]}:${RAFT_PORT}")
    PEER_CONTROL_ARGS+=(--peer-control "${nid}=${PRIVATE_IP[$i]}:${CONTROL_PORT}")
    HOST_ARGS+=(--host "${nid}@${PRIVATE_IP[$i]}:${DATA_PORT}:${CONTROL_PORT}")
done
VOTERS="$(seq -s, 1 "${NODES}")"

echo "[step] Start ${NODES} hosts under systemd-run"
# systemd-run, never `ssh host "cmd &"` (doctrine 136): a backgrounded remote
# command still holds the ssh session channel open no matter how its own
# streams are redirected, so the ssh does not return until the command exits.
# systemd-run hands the process to pid 1 and returns immediately; the
# controlling side then polls a state artifact -- here /ready on the control
# port -- rather than the unit's own status.
for (( i = 0; i < NODES; ++i )); do
    nid=$(( i + 1 ))
    node_args="--node-id ${nid} --bind 0.0.0.0 \
        --raft-port ${RAFT_PORT} --data-port ${DATA_PORT} --control-port ${CONTROL_PORT} \
        --voters ${VOTERS} --groups ${GROUP_COUNT} --key-count ${KEY_SPACE} \
        --tick-interval ${TICK_INTERVAL} --transport ${TRANSPORT} \
        --persistence ${PERSISTENCE} --data-threads ${DATA_THREADS} \
        ${PEER_ARGS[*]} ${PEER_CONTROL_ARGS[*]}"
    if [[ "${PERSISTENCE}" != "memory" ]]; then
        node_args="${node_args} --data-dir /tmp/kythira-data"
    fi
    ssh_to "$i" "rm -f /tmp/node.log && mkdir -p /tmp/kythira-data && \
        sudo systemd-run --unit=kythira-multi-raft-node --collect \
            --property=User=ubuntu \
            --property=WorkingDirectory=/tmp \
            --property=LimitNOFILE=65536 \
            --property=StandardOutput=append:/tmp/node.log \
            --property=StandardError=append:/tmp/node.log \
            /tmp/${NODE_BIN_NAME} ${node_args}" >/dev/null
    echo "  host ${nid} started on ${PRIVATE_IP[$i]}"
done

echo "[step] Wait for /ready on every host"
# /ready means EVERY GROUP HAS A LEADER, not that the process is up. A driver
# that starts before that measures the election.
READY_DEADLINE=$(( $(date +%s) + 180 ))
for (( i = 0; i < NODES; ++i )); do
    nid=$(( i + 1 ))
    while true; do
        body=$(ssh_to "$i" "curl -sf --max-time 5 http://127.0.0.1:${CONTROL_PORT}/ready" 2>/dev/null || true)
        if [[ "${body}" == *'"ready":true'* ]]; then
            echo "  host ${nid} ready"
            break
        fi
        if [[ $(date +%s) -ge ${READY_DEADLINE} ]]; then
            # Requirement 3.5's shape, applied to readiness: name what was
            # never seen rather than measuring a cluster with a replica
            # missing and reporting a number for it.
            echo "ERROR: host ${nid} was not ready within 180s. Last: ${body:-no response}" >&2
            echo "--- its log ---" >&2
            ssh_to "$i" "tail -40 /tmp/node.log" >&2 2>/dev/null || true
            # `head -c` FIRST, and that bound is not caution — it is the same
            # non-termination the success path hit. The hosts are still
            # running here (that is what failed), so the log is still growing
            # at tick rate and a plain read to EOF never finishes. `head`
            # bounds it deterministically, and it takes the log from the
            # START because a host that never reached /ready failed during
            # startup and the beginning is where that is visible.
            for (( j = 0; j < NODES; ++j )); do
                ssh_to "$j" "head -c 50000000 /tmp/node.log | gzip -9 -c > /tmp/node.log.gz" \
                    2>/dev/null || true
                scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$j]}:/tmp/node.log.gz" \
                    "${OUT_DIR}/node-$(( j + 1 )).log.gz" 2>/dev/null || true
            done
            exit 1
        fi
        sleep 5
    done
done

# ── The network, measured before the window (Requirement 4) ──────────────────
echo "[step] Probe the network between every ordered pair"
# /probe already does what Requirement 4.1 and 4.2 ask: RTT to each peer as
# the MEDIAN of eleven samples (not the mean -- one scheduling hiccup moves a
# mean, and a network figure a single outlier can move is not one to size a
# cluster from), plus a 1 MiB echo for bandwidth. Called on every host, so
# what is recorded is the MATRIX and not an average: a cross-AZ cluster is not
# symmetric and an average hides which link is slow.
for (( i = 0; i < NODES; ++i )); do
    nid=$(( i + 1 ))
    ssh_to "$i" "curl -sf --max-time 120 http://127.0.0.1:${CONTROL_PORT}/probe" \
        > "${OUT_DIR}/probe-node-${nid}.json" 2>/dev/null || echo '{}' > "${OUT_DIR}/probe-node-${nid}.json"
    echo "  node ${nid}: $(head -c 200 "${OUT_DIR}/probe-node-${nid}.json")"
done

# Requirement 4.5's sanity check needs a loopback reference to compare
# against, and the honest way to get one is to measure it HERE, with the same
# client and the same request, rather than to hardcode a threshold.
#
# Why not reuse /probe for it: /probe deliberately skips the host itself
# (a peer is not oneself), and changing that would be a change to the host
# binary, which this spec puts out of scope. So the baseline is measured with
# curl, and -- because the comparison is only meaningful between like
# measurements -- the PEER side of the ratio is measured with curl too. The
# authoritative matrix on the row remains /probe's; these two numbers exist
# only to answer "is this loopback wearing a network's clothes".
#
# curl is given every URL in one invocation so it reuses the connection, as
# /probe's keep-alive client does. Measuring the baseline with a fresh TCP
# handshake per sample would inflate it and bias the ratio towards declaring a
# real network container-shaped.
echo "[step] Measure a loopback baseline for Requirement 4.5"
LOOPBACK_US=""
PEERWISE_US=""
if (( NODES >= 2 )); then
    # `-o /dev/null` is POSITIONAL: curl pairs each -o with the next URL, so a
    # single one covers only the FIRST of eleven and the other ten bodies land
    # on stdout, interleaved with the timings. awk then reads a JSON line as a
    # number, gets 0, and the median comes back 0.0 -- which is what the first
    # live run of this produced. One -o per URL.
    self_args=""
    peer_args=""
    for _ in $(seq 1 11); do
        self_args="${self_args} -o /dev/null http://127.0.0.1:${CONTROL_PORT}/health"
        peer_args="${peer_args} -o /dev/null http://${PRIVATE_IP[1]}:${CONTROL_PORT}/health"
    done
    # Median of eleven, by the same rule /probe uses: one scheduling hiccup
    # moves a mean and does not move a median. Non-numeric lines are DROPPED
    # rather than read as zero, so a partial failure shrinks the sample instead
    # of dragging the median to nothing and reporting an instantaneous network.
    measure_rtt() {
        ssh_to 0 "curl -s -w '%{time_total}\n' $1" 2>/dev/null \
            | awk '$1 ~ /^[0-9]*\.?[0-9]+$/ { printf "%.1f\n", $1 * 1000000 }' \
            | sort -n \
            | awk '{a[NR] = $1} END { if (NR) printf "%.1f", a[int((NR + 1) / 2)] }'
    }
    # `|| true`: a failed probe must not end a run whose measured phase has not
    # happened yet. An absent baseline is reported as unknown, which is
    # Requirement 18.4's rule -- null, never guessed.
    LOOPBACK_US=$(measure_rtt "${self_args}" || true)
    PEERWISE_US=$(measure_rtt "${peer_args}" || true)
    echo "  node 1 -> itself:  ${LOOPBACK_US:-unknown} us (median of 11)"
    echo "  node 1 -> node 2:  ${PEERWISE_US:-unknown} us (median of 11)"
fi

NETWORK_VERDICT="unknown"
NETWORK_RATIO=""
if [[ -n "${LOOPBACK_US}" && -n "${PEERWISE_US}" ]] \
   && python3 -c "import sys; sys.exit(0 if ${LOOPBACK_US} > 0 else 1)" 2>/dev/null; then
    NETWORK_RATIO=$(python3 -c "print('%.2f' % (${PEERWISE_US} / ${LOOPBACK_US}))" 2>/dev/null || echo "")
    if [[ -n "${NETWORK_RATIO}" ]]; then
        # Requirement 4.5, verbatim: within an order of magnitude of loopback
        # means this is a container-shaped row and must SAY so. That is the
        # exact failure the Podman rows demonstrated, and it is silent --
        # nothing about a container stack reports that the "network" between
        # two nodes is a memcpy.
        if python3 -c "import sys; sys.exit(0 if ${NETWORK_RATIO} < 10.0 else 1)"; then
            NETWORK_VERDICT="container-shaped"
            echo ""
            echo "  !!! Requirement 4.5: the inter-node round trip is ${NETWORK_RATIO}x this"
            echo "  !!! host's own loopback, which is WITHIN AN ORDER OF MAGNITUDE."
            echo "  !!! This row must be read as a CONTAINER-SHAPED row, not a"
            echo "  !!! multi-machine one. The row records that verdict."
            echo ""
        else
            NETWORK_VERDICT="multi-machine"
            echo "  Requirement 4.5: ${NETWORK_RATIO}x loopback — a real network."
        fi
    fi
fi

# Every zone verbatim on the row (Requirement 2.2), not in a commit message.
# Built up in pieces rather than one interpolated string. A `$( [[ test ]] &&
# printf ... )` inside an assignment makes the ASSIGNMENT fail when the test is
# false -- the command substitution's status becomes the assignment's -- and
# under `set -e` that silently ended the first live run of this script between
# the network probe and the measured phase, after provisioning four instances.
PLACEMENT_ZONES=""
for (( i = 0; i < NODES; ++i )); do
    PLACEMENT_ZONES="${PLACEMENT_ZONES}$(( i + 1 ))=${REAL_AZ[$i]} "
done
PLACEMENT_PG=""
if [[ "${PG_CREATED}" == "1" ]]; then
    PLACEMENT_PG="; placement group ${PG_NAME} (cluster)"
fi
PLACEMENT_TEXT="${PLACEMENT}; region ${REGION}; hosts ${PLACEMENT_ZONES}| driver ${DRIVER_INSTANCE_TYPE} in ${REAL_AZ[$NODES]}${PLACEMENT_PG}; inter-node RTT ${PEERWISE_US:-unknown} us vs loopback ${LOOPBACK_US:-unknown} us (${NETWORK_VERDICT})"

echo "[step] Measured phase: ${REPETITIONS} repetitions (ceiling ${CEILING_MINUTES}m)"
echo "  placement recorded as: ${PLACEMENT_TEXT}"
BENCH_RC=0
set +e
ssh_to "${NODES}" "cd /tmp && mkdir -p /tmp/results && \
    timeout --signal=KILL ${CEILING_MINUTES}m /tmp/${BENCH_BIN_NAME} \
      $(printf '%q ' "${HOST_ARGS[@]}") \
      --groups ${GROUP_COUNT} --key-space ${KEY_SPACE} \
      --tier e --placement $(printf '%q' "${PLACEMENT_TEXT}") \
      --transport ${TRANSPORT} --durability ${PERSISTENCE} \
      --tick-interval ${TICK_INTERVAL} --operations ${OPERATIONS} \
      --in-flight ${IN_FLIGHT} --value-bytes ${VALUE_BYTES} \
      --repetitions ${REPETITIONS} --scenario ${SCENARIO} \
      --axis tier-e --out-dir /tmp/results \
      > /tmp/bench.log 2>&1; echo \$? > /tmp/bench.rc"
BENCH_SSH_RC=$?
set -e
if [[ ${BENCH_SSH_RC} -ne 0 ]]; then
    echo "  WARNING: the ssh carrying the measured phase exited ${BENCH_SSH_RC}." >&2
fi

# ── Collect EVERYTHING before teardown (Requirement 1.5) ─────────────────────
# A Tier E failure is diagnosed from the log of the machine that failed, and
# there is no second chance: the instances are about to be destroyed. Pulled
# unconditionally, before any judgement about success, and with `|| true` so
# that one unreachable host does not cost the artifacts of the others.
# ── Stop the hosts BEFORE collecting their logs ──────────────────────────────
# Not tidiness — correctness. `multi_raft` has no timer of its own, so the host
# drives tick() on a thread at --tick-interval, and every tick logs. An idle
# three-node cluster at 2 ms therefore writes megabytes a minute FOREVER, and
# the first live run of this script discovered what that means for collection:
# scp reads to EOF, the writer appends faster than a home downstream can pull,
# and the copy never terminates. The log went 46 MB -> 78 MB while being
# fetched. A measured phase that finished in seconds was followed by a
# collection that could not finish at all.
#
# Stopping the unit first makes the log a fixed-size object. It is safe here
# because the measured phase is over and the Tier C arm, if any, runs entirely
# on the driver instance with host processes of its own.
echo "[step] Stop the hosts so their logs stop growing"
for (( i = 0; i < NODES; ++i )); do
    ssh_to "$i" "sudo systemctl stop kythira-multi-raft-node 2>/dev/null; true" >/dev/null 2>&1 || true
done

echo "[step] Collect artifacts from all ${TOTAL_INSTANCES} instances"
ssh_to "${NODES}" "sed -e 's/\x1b\[[0-9;]*m//g' /tmp/bench.log > /tmp/bench.clean.log" 2>/dev/null || true
scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/bench.clean.log" "${OUT_DIR}/" || true
scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/bench.rc" "${OUT_DIR}/" || true
scp "${SSH_OPTS[@]}" -qr "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/results/." "${OUT_DIR}/" || true
# COMPRESSED ON THE INSTANCE, not pulled raw. A host's log is tens of
# megabytes for a few hundred operations — the first live run produced 46 MB
# each — and pulling N of those over the controlling machine's downstream took
# longer than the measured phase, the deploy and the boot combined. gzip on an
# idle 8-vCPU instance is seconds and the text is highly repetitive, so this is
# most of an order of magnitude off the wall clock for no loss: Requirement 1.5
# asks for every host's log, not for it uncompressed.
for (( i = 0; i < NODES; ++i )); do
    ssh_to "$i" "gzip -9 -c /tmp/node.log > /tmp/node.log.gz" 2>/dev/null || true
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$i]}:/tmp/node.log.gz" \
        "${OUT_DIR}/node-$(( i + 1 )).log.gz" || true
done
BENCH_RC=$(cat "${OUT_DIR}/bench.rc" 2>/dev/null || echo unknown)
echo "  driver exited ${BENCH_RC}; artifacts in ${OUT_DIR}/"

TIER_C_TAKEN=0
if [[ "${ALSO_TIER_C}" == "1" ]]; then
    echo "[step] Tier C arm on the driver instance — the delta task 6 asks for"
    # Same binaries, same workload, same instance type. The ONLY difference
    # from the Tier E row above is that all N host processes and the driver now
    # share one machine, which is precisely the variable this spec exists to
    # isolate. run-tier-c-row.sh is reused verbatim rather than reimplemented
    # here so that the Tier C row is the same row the local launcher produces.
    set +e
    ssh_to "${NODES}" "mkdir -p /tmp/bin /tmp/tierc && \
        cp /tmp/${NODE_BIN_NAME} /tmp/bin/multi_raft_node && \
        cp /tmp/${BENCH_BIN_NAME} /tmp/bin/multi_raft_bench && \
        timeout --signal=KILL ${CEILING_MINUTES}m /tmp/run-tier-c-row.sh \
          --build-dir /tmp/bin --nodes ${NODES} --groups ${GROUP_COUNT} \
          --operations ${OPERATIONS} --in-flight ${IN_FLIGHT} \
          --value-bytes ${VALUE_BYTES} --repetitions ${REPETITIONS} \
          --scenario ${SCENARIO} --tick-interval ${TICK_INTERVAL} \
          --transport ${TRANSPORT} --persistence ${PERSISTENCE} \
          --data-threads ${DATA_THREADS} --out-dir /tmp/tierc \
          > /tmp/tierc.log 2>&1; echo \$? > /tmp/tierc.rc"
    set -e
    ssh_to "${NODES}" "sed -e 's/\x1b\[[0-9;]*m//g' /tmp/tierc.log > /tmp/tierc.clean.log" 2>/dev/null || true
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/tierc.clean.log" "${OUT_DIR}/" || true
    mkdir -p "${OUT_DIR}/tier-c"
    scp "${SSH_OPTS[@]}" -qr "ubuntu@${PUBLIC_IP[$NODES]}:/tmp/tierc/." "${OUT_DIR}/tier-c/" || true
    TIER_C_RC=$(ssh_to "${NODES}" "cat /tmp/tierc.rc 2>/dev/null" 2>/dev/null || echo unknown)
    echo "  Tier C arm exited ${TIER_C_RC}; artifacts in ${OUT_DIR}/tier-c/"
    [[ "${TIER_C_RC}" == "0" ]] && TIER_C_TAKEN=1
fi

python3 - "${OUT_DIR}/run.json" <<PY
import json, sys
azs = "${REAL_AZ[*]}".split()
json.dump({
    "shape": "aws-shape-2",
    "tier": "e",
    "run_tag": "${RUN_TAG}",
    "region": "${REGION}",
    "placement": "${PLACEMENT}",
    "placement_group": "${PLACEMENT_GROUP_NAME}" or None,
    "placement_verbatim": """${PLACEMENT_TEXT}""",
    "nodes": ${NODES},
    "instances_total": ${TOTAL_INSTANCES},
    "host_instance_type": "${INSTANCE_TYPE}",
    "driver_instance_type": "${DRIVER_INSTANCE_TYPE}",
    "host_availability_zones": azs[:${NODES}],
    "driver_availability_zone": azs[${NODES}] if len(azs) > ${NODES} else None,
    "instance_ids": "${INSTANCE_IDS[*]}".split(),
    "image_id": "${AMI_ID}",
    "arch": "${ARCH}",
    "discovery": "${DISCOVERY}",
    "network": {
        "loopback_rtt_us": ${LOOPBACK_US:-None},
        "inter_node_rtt_us": ${PEERWISE_US:-None},
        "ratio_to_loopback": ${NETWORK_RATIO:-None},
        # Requirement 4.5. "container-shaped" means the measured inter-node
        # round trip is within an order of magnitude of this host's own
        # loopback, so the row describes containers on one machine however
        # many machines were paid for.
        "verdict": "${NETWORK_VERDICT}",
        "probe_matrix_files": ["probe-node-%d.json" % (i + 1) for i in range(${NODES})],
    },
    "workload": {
        "scenario": "${SCENARIO}", "groups": ${GROUP_COUNT},
        "operations": ${OPERATIONS}, "in_flight": ${IN_FLIGHT},
        "value_bytes": ${VALUE_BYTES}, "repetitions": ${REPETITIONS},
        "tick_interval_ms": ${TICK_INTERVAL}, "transport": "${TRANSPORT}",
        "persistence": "${PERSISTENCE}", "data_threads": ${DATA_THREADS},
    },
    "durability_note": (
        "persistence=memory: comparable to an external NO-FSYNC number and to "
        "nothing else (Requirement 6.2). The durable comparison needs Tier D."
        if "${PERSISTENCE}" == "memory" else
        "persistence=${PERSISTENCE}"),
    "benchmark_exit_code": "${BENCH_RC}",
    # Task 6: a Tier C row taken on ONE of these instances, so the Tier C/Tier
    # E delta differs in placement and not in hardware.
    "tier_c_comparison_taken": bool(${TIER_C_TAKEN}),
    "node_binary_sha256": "${NODE_SHA}",
    "bench_binary_sha256": "${BENCH_SHA}",
    "binary_origin": "shipped from the controlling machine",
    "git_commit": "$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)",
}, open(sys.argv[1], "w"), indent=2)
PY
echo "  ${OUT_DIR}/run.json"

if [[ "${BENCH_RC}" != "0" ]]; then
    echo "ERROR: the measured phase exited ${BENCH_RC}. See ${OUT_DIR}/bench.clean.log." >&2
    exit 1
fi
echo ""
echo "Measured phase completed. Teardown and audit follow."
