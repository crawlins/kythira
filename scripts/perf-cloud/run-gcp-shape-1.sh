#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Shape 1 on GCP: one non-burstable Compute Engine instance, Tier B, running
# the same benchmark binary as the AWS rows and the local ones
# (.kiro/specs/multi-raft-performance/ Requirement 18.12).
#
# The sibling of run-aws-shape-1.sh and deliberately a separate script. The
# two providers agree on almost nothing that matters here: GCE has no key
# pairs (SSH keys are instance metadata), no security groups on the instance
# (firewall rules are network-scoped and cannot carry labels), its boot disk
# is a zonal child of the instance rather than a tagged volume, and it has a
# control-plane dead-man switch AWS lacks. A shared abstraction over that
# would be a third thing to debug at 2 a.m. with an instance billing.
#
# What IS shared is the shape, deliberately: one run-scoped identifier
# applied at creation, an unconditional teardown on an EXIT trap so the
# failure paths are covered, and a leak audit that is a separate claim from
# the teardown.
#
# Usage:
#   scripts/perf-cloud/run-gcp-shape-1.sh --binary PATH [OPTIONS]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

BINARY=""
MACHINE_TYPE="n2-standard-8"
ZONE="us-central1-a"
PROJECT="${CLOUDSDK_CORE_PROJECT:-}"
RUN_TAG=""
FILTER="multi_raft_http_benchmark/write_throughput_by_rpc_serializer"
CEILING_MINUTES="30"
REPEAT=1
OUT_DIR="perf-cloud-results-gcp"
DRY_RUN=0
KEEP=0

LABEL_KEY="kythira-perf-run"

usage() {
    cat <<'EOF'
Usage: run-gcp-shape-1.sh --binary PATH [OPTIONS]

Provisions one non-burstable Compute Engine instance, ships a prebuilt
benchmark binary to it, runs the measured phase under a hard ceiling, pulls
the artifacts back, and tears everything down unconditionally.

Required:
  --binary PATH             Prebuilt multi_raft_http_benchmark_test, x86-64.

Optional:
  --machine-type TYPE       default: n2-standard-8 (8 vCPU, to match the AWS
                             rows). Shared-core types are refused.
  --zone ZONE               default: us-central1-a
  --project ID              default: $CLOUDSDK_CORE_PROJECT
  --run-tag VALUE           default: perf-local-<epoch>-<pid>. Applied as a
                             LABEL at creation; the audit keys off it.
  --filter FILTER           Boost.Test --run_test filter for the measured phase
  --ceiling-minutes N       Hard ceiling per repetition. default: 30
  --repeat N                Repetitions on the one instance. default: 1
  --out-dir DIR             default: perf-cloud-results-gcp
  --keep                    Skip teardown. DEBUGGING ONLY; it keeps billing.
  --dry-run                 Print what would run without provisioning
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) BINARY="$2"; shift 2 ;;
        --machine-type) MACHINE_TYPE="$2"; shift 2 ;;
        --zone) ZONE="$2"; shift 2 ;;
        --project) PROJECT="$2"; shift 2 ;;
        --run-tag) RUN_TAG="$2"; shift 2 ;;
        --filter) FILTER="$2"; shift 2 ;;
        --ceiling-minutes) CEILING_MINUTES="$2"; shift 2 ;;
        --repeat) REPEAT="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "${BINARY}" ]]; then
    echo "ERROR: --binary is required." >&2; usage >&2; exit 1
fi
if [[ ! -f "${BINARY}" ]]; then
    echo "ERROR: no such file: ${BINARY}" >&2; exit 1
fi
if [[ -z "${PROJECT}" ]]; then
    echo "ERROR: no project. Pass --project or set CLOUDSDK_CORE_PROJECT." >&2; exit 1
fi

# Requirement 18.5, enforced here as well as on the instance. GCP's burstable
# and shared-core families are e2-micro/small/medium and the whole f1/g1
# legacy line; capture-provenance.sh carries the same table and is the
# authority. Refusing before the instance exists costs nothing.
case "${MACHINE_TYPE}" in
    e2-micro|e2-small|e2-medium|f1-*|g1-*)
        echo "ERROR: '${MACHINE_TYPE}' is a shared-core/burstable type. A benchmark that" \
             "exhausts its CPU allowance measures the allowance, not the machine" \
             "(Requirement 18.5)." >&2
        exit 1 ;;
esac

BIN_ARCH=$(LC_ALL=C file -b "${BINARY}" | sed -n 's/.*ELF 64-bit LSB[^,]*, \([^,]*\),.*/\1/p')
if [[ "${BIN_ARCH}" != "x86-64" ]]; then
    echo "ERROR: ${BINARY} is '${BIN_ARCH}'. This script provisions x86-64 machine types;" \
         "a Tau T2A (arm64) row would need its own image family and is not wired here." >&2
    exit 1
fi

# GCE labels are lowercase alphanumeric, dash and underscore only, so the run
# tag has a narrower alphabet than AWS's tag values do.
if [[ -z "${RUN_TAG}" ]]; then
    RUN_TAG="perf-local-$(date -u +%s)-$$"
fi
if ! [[ "${RUN_TAG}" =~ ^[a-z0-9_-]{1,63}$ ]]; then
    echo "ERROR: '${RUN_TAG}' is not a valid GCE label value (lowercase alphanumeric," \
         "dash and underscore, 63 characters or fewer)." >&2
    exit 1
fi

echo "=== Shape 1, GCP ==========================================="
echo "  project:      ${PROJECT}"
echo "  zone:         ${ZONE}"
echo "  machine type: ${MACHINE_TYPE}"
echo "  run label:    ${LABEL_KEY}=${RUN_TAG}"
echo "  binary:       ${BINARY} ($(stat -c %s "${BINARY}") bytes, ${BIN_ARCH})"
echo "  repeat:       ${REPEAT}"
echo "  ceiling:      ${CEILING_MINUTES} minutes (per repetition)"
echo "============================================================"

if [[ "${DRY_RUN}" == "1" ]]; then
    echo "[dry-run] would create one ${MACHINE_TYPE} in ${ZONE} labelled"
    echo "[dry-run] ${LABEL_KEY}=${RUN_TAG}, ship ${BINARY}, run the measured phase,"
    echo "[dry-run] pull results into ${OUT_DIR}, then delete and audit."
    exit 0
fi

WORK_DIR=$(mktemp -d)
INSTANCE_NAME="kythira-${RUN_TAG}"
INSTANCE_CREATED=0

gcloud_q() { gcloud --project "${PROJECT}" --quiet "$@"; }

# Unconditional teardown (Requirement 18.10), on an EXIT trap rather than a
# trailing block: the run most likely to have leaked is the one that failed
# partway, which is exactly when a trailing block does not execute.
teardown() {
    local rc=$?
    set +e
    if [[ "${KEEP}" == "1" ]]; then
        echo ""
        echo "!!! --keep given: NOT tearing down. This is billing now:"
        echo "!!!   instance: ${INSTANCE_NAME} in ${ZONE}"
        exit $rc
    fi
    echo ""
    echo "=== Teardown ============================================="
    if [[ "${INSTANCE_CREATED}" == "1" ]]; then
        echo "  deleting ${INSTANCE_NAME}"
        # --delete-disks=all rather than trusting auto-delete. It is the
        # default for a boot disk created from an image, but a default is not
        # a control, and an orphaned pd-balanced bills indefinitely.
        gcloud_q compute instances delete "${INSTANCE_NAME}" \
            --zone "${ZONE}" --delete-disks=all >/dev/null 2>&1
    fi
    rm -rf "${WORK_DIR}"
    echo "=== Audit ================================================"
    # A separate claim from the teardown, and independent of it: "the
    # teardown ran" and "nothing is left running" are different statements.
    bash "${REPO_ROOT}/scripts/perf-cloud/audit-gcp-leaks.sh" "${RUN_TAG}" "${PROJECT}"
    local audit_rc=$?
    if [[ $rc -eq 0 && $audit_rc -ne 0 ]]; then rc=$audit_rc; fi
    # `exit`, not `return`: an EXIT trap that returns leaves the script's
    # status as it was, so a clean run followed by a FAILING audit would
    # exit 0 — the one outcome this must never report.
    exit $rc
}
trap 'teardown' EXIT

echo ""
echo "[step] Read the machine type's stated facts"
# The control-plane half of Requirement 18.4, read where the credentials are
# so the instance needs none of its own.
STATED=$(gcloud_q compute machine-types describe "${MACHINE_TYPE}" --zone "${ZONE}" \
    --format 'value(guestCpus,memoryMb)')
STATED_VCPU=$(cut -f1 <<<"${STATED}")
STATED_MEM=$(cut -f2 <<<"${STATED}")
echo "  ${STATED_VCPU} vCPU, ${STATED_MEM} MiB stated"

echo "[step] Generate an ephemeral SSH key"
# Instance metadata, never project metadata. `gcloud compute ssh` would push a
# key into the PROJECT's metadata, where it outlives this run and grants
# access to every instance in the project that accepts project keys. An
# instance-scoped key dies with the instance and cannot leak.
ssh-keygen -t ed25519 -N '' -q -f "${WORK_DIR}/id_ed25519" -C kythira-perf
printf 'ubuntu:%s\n' "$(cat "${WORK_DIR}/id_ed25519.pub")" > "${WORK_DIR}/ssh-keys"

echo "[step] Create the instance"
# The dead-man switch, and GCE's is better than the cloud-init `shutdown -h`
# the AWS script has to use: --max-run-duration with
# --instance-termination-action=DELETE is enforced by the CONTROL PLANE, so
# it fires even if the guest never boots, and it DELETES rather than stopping
# — a stopped GCE instance still bills its boot disk. It needs no credentials
# on the instance, which keeps Requirement 18.4's "the instance needs no
# cloud permissions" intact.
DEADMAN_MINUTES=$(( CEILING_MINUTES * REPEAT + 30 ))
echo "  control-plane deletion scheduled at +${DEADMAN_MINUTES} minutes"
gcloud_q compute instances create "${INSTANCE_NAME}" \
    --zone "${ZONE}" \
    --machine-type "${MACHINE_TYPE}" \
    --image-family ubuntu-2404-lts-amd64 \
    --image-project ubuntu-os-cloud \
    --boot-disk-type pd-balanced \
    --boot-disk-size 20GB \
    --boot-disk-auto-delete \
    --labels "${LABEL_KEY}=${RUN_TAG}" \
    --metadata-from-file "ssh-keys=${WORK_DIR}/ssh-keys" \
    --max-run-duration "${DEADMAN_MINUTES}m" \
    --instance-termination-action DELETE \
    --no-service-account --no-scopes \
    --format 'value(name)' >/dev/null
INSTANCE_CREATED=1
echo "  ${INSTANCE_NAME}"

PUBLIC_IP=$(gcloud_q compute instances describe "${INSTANCE_NAME}" --zone "${ZONE}" \
    --format 'value(networkInterfaces[0].accessConfigs[0].natIP)')
# The DISK resource, not the instance's view of it. `instances describe`
# reports `disks[0].type` as the attachment kind — "PERSISTENT" — which is the
# interface and not the class. Requirement 3.4 asks for the volume class, and
# "PERSISTENT" would satisfy nobody comparing a pd-balanced row against a
# pd-ssd one. The first live GCP run recorded PERSISTENT for exactly this
# reason.
BOOT_DISK=$(gcloud_q compute disks describe "${INSTANCE_NAME}" --zone "${ZONE}" \
    --format 'value(type)' | sed 's#.*/##')
echo "  ${PUBLIC_IP}, boot disk ${BOOT_DISK}"

SSH_OPTS=(-i "${WORK_DIR}/id_ed25519" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=10 -o ServerAliveInterval=30)

echo "[step] Wait for SSH"
for attempt in $(seq 1 60); do
    if ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" true 2>/dev/null; then
        echo "  up after $((attempt * 5))s"; break
    fi
    if [[ "${attempt}" == "60" ]]; then
        echo "ERROR: sshd never answered on ${PUBLIC_IP} within 300s." >&2
        echo "--- serial console ---" >&2
        gcloud_q compute instances get-serial-port-output "${INSTANCE_NAME}" \
            --zone "${ZONE}" 2>/dev/null | tail -40 >&2
        exit 1
    fi
    sleep 5
done

echo "[step] Ship the provenance script and the binary"
BIN_NAME=$(basename "${BINARY}")
scp "${SSH_OPTS[@]}" -q "${BINARY}" \
    "${REPO_ROOT}/scripts/perf-cloud/capture-provenance.sh" \
    "ubuntu@${PUBLIC_IP}:/tmp/"
ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "chmod +x /tmp/${BIN_NAME} /tmp/capture-provenance.sh"

echo "[step] Capture provenance (Requirement 18.4)"
# GCE serves provider, zone, machine type, image and instance id from its own
# metadata server, which capture-provenance.sh probes directly — those are
# guest-observed and are not passed in. What IS passed is the control-plane
# half GCE does not serve to the guest: the disk type, and the tenancy, which
# on GCE is "shared" unless sole-tenancy was requested and is stated rather
# than assumed.
mkdir -p "${OUT_DIR}"
ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
    "KYTHIRA_PERF_STATED_STORAGE_CLASS='${BOOT_DISK}' \
     KYTHIRA_PERF_STATED_TENANCY='shared (no sole-tenant node affinity requested)' \
     KYTHIRA_PERF_STATED_INSTANCE_TYPE='${MACHINE_TYPE}' \
     /tmp/capture-provenance.sh /tmp/provenance.json"
scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/provenance.json" "${OUT_DIR}/" || true

RCS=()
for rep in $(seq 1 "${REPEAT}"); do
    echo "[step] Measured phase ${rep}/${REPEAT} (ceiling ${CEILING_MINUTES} minutes)"
    set +e
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
        "cd /tmp && timeout --signal=KILL ${CEILING_MINUTES}m ./${BIN_NAME} \
           --run_test='${FILTER}' --log_level=test_suite > /tmp/run${rep}.log 2>&1; \
         echo \$? > /tmp/run${rep}.rc"
    set -e
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" \
        "sed -e 's/\x1b\[[0-9;]*m//g' /tmp/run${rep}.log > /tmp/run${rep}.clean.log" || true
    scp "${SSH_OPTS[@]}" -q "ubuntu@${PUBLIC_IP}:/tmp/run${rep}.clean.log" "${OUT_DIR}/" || true
    rc=$(ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "cat /tmp/run${rep}.rc" 2>/dev/null || echo unknown)
    RCS+=("${rc}")
    echo "  repetition ${rep} exited ${rc}"
    ssh "${SSH_OPTS[@]}" "ubuntu@${PUBLIC_IP}" "rm -f /tmp/run${rep}.log" || true
done

BENCH_RC="${RCS[0]}"
for rc in "${RCS[@]}"; do
    if [[ "${rc}" != "0" ]]; then BENCH_RC="${rc}"; fi
done
echo "  artifacts in ${OUT_DIR}/ (exit codes: ${RCS[*]})"

python3 - "${OUT_DIR}/run.json" <<PY
import json, sys
json.dump({
    "shape": "gcp-shape-1",
    "provider": "gcp",
    "run_tag": "${RUN_TAG}",
    "project": "${PROJECT}",
    "zone": "${ZONE}",
    "machine_type": "${MACHINE_TYPE}",
    "instance_name": "${INSTANCE_NAME}",
    "boot_disk_type": "${BOOT_DISK}",
    "stated_vcpu": "${STATED_VCPU}",
    "stated_memory_mib": "${STATED_MEM}",
    "benchmark_filter": "${FILTER}",
    "ceiling_minutes": ${CEILING_MINUTES},
    "repetitions": ${REPEAT},
    "benchmark_exit_codes": "${RCS[*]}",
    "binary_sha256": "$(sha256sum "${BINARY}" | cut -d' ' -f1)",
    "binary_bytes": $(stat -c %s "${BINARY}"),
    "binary_origin": "shipped from the controlling machine",
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
