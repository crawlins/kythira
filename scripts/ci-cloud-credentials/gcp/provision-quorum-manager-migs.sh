#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Creates the managed instance groups the gcp_mig_real_gce suite in
# tests/gcp_quorum_manager_real_gce_test.cpp runs against. Until this script
# existed nothing created them, the workflow passed no MIG names, and all six
# MIG cases skipped on every run.
#
# Everything here is free while idle: an instance template, a TCP health
# check and two zonal MIGs at size 0. Instances exist only while a case has
# grown a group.
#
#   kythira-it-mig-a         the group the manager drives. No autohealing,
#                            which the manager requires (Requirement 18).
#   kythira-it-mig-autoheal  a group with an autohealing policy, so
#                            mig_construction_rejects_autohealing_policy has
#                            something to reject. It is never resized.
#
# Instances are named kythira-kythira-it-mig-*, inside the
# kythira-kythira-it-* prefix the workflow's leak audit lists, so a group
# left grown by a failed case shows up there.
#
# Safe to re-run: every resource is created only if absent. An existing MIG is
# never resized here, since that would delete whatever it holds; a non-zero
# size is reported instead. kythira-it-mig-a has its autohealing cleared on
# every run, so a hand edit cannot leave the manager refusing it.
#
# Usage:
#   scripts/ci-cloud-credentials/gcp/provision-quorum-manager-migs.sh \
#       [--project ID] [--zone ZONE] [--machine-type TYPE] [--network NET]
#       [--dry-run]
set -euo pipefail

PROJECT=""
ZONE="us-central1-a"
MACHINE_TYPE="e2-micro"
NETWORK="default"
IMAGE_FAMILY="debian-12"
IMAGE_PROJECT="debian-cloud"
TEMPLATE="kythira-it-mig-template"
HEALTH_CHECK="kythira-it-mig-hc"
MIG="kythira-it-mig-a"
MIG_AUTOHEAL="kythira-it-mig-autoheal"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-quorum-manager-migs.sh [OPTIONS]

Creates (if absent) the instance template, health check and the two zonal
MIGs the real-GCE MIG cases need. Safe to re-run.

Optional:
  --project ID         default: the gcloud config's current project
  --zone ZONE          default: us-central1-a. The suite uses
                       <GCP_REAL_CLOUD_TESTS_REGION>-a, so keep them in step.
  --machine-type TYPE  default: e2-micro
  --network NET        default: default (the network the Compute cases use)
  --dry-run            print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) PROJECT="$2"; shift 2 ;;
        --zone) ZONE="$2"; shift 2 ;;
        --machine-type) MACHINE_TYPE="$2"; shift 2 ;;
        --network) NETWORK="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        printf 'DRY-RUN:'; printf ' %q' "$@"; printf '\n'
        return 0
    fi
    "$@"
}

: "${PROJECT:=$(gcloud config get-value project 2>/dev/null)}"
if [[ -z "${PROJECT}" || "${PROJECT}" == "(unset)" ]]; then
    echo "no project: pass --project or set one with 'gcloud config set project'" >&2
    exit 2
fi

echo "project: ${PROJECT}"
echo "zone:    ${ZONE}"

# ── 1. Instance template ─────────────────────────────────────────────────────
# No external address and no service account: the suite reads instance state
# and never connects to an instance, matching the Compute cases' instances.
# Templates are immutable, so an existing one is left as it is.
if gcloud compute instance-templates describe "${TEMPLATE}" --project="${PROJECT}" \
    --format="value(name)" >/dev/null 2>&1; then
    echo "instance template ${TEMPLATE} exists — leaving it alone"
else
    echo "creating instance template ${TEMPLATE}"
    run gcloud compute instance-templates create "${TEMPLATE}" \
        --project="${PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --image-family="${IMAGE_FAMILY}" \
        --image-project="${IMAGE_PROJECT}" \
        --network="${NETWORK}" \
        --no-address \
        --no-service-account \
        --no-scopes \
        --labels=kythira-suite=gcp-mig-quorum-manager
fi

# ── 2. The managed group, without autohealing ────────────────────────────────
if gcloud compute instance-groups managed describe "${MIG}" --project="${PROJECT}" \
    --zone="${ZONE}" --format="value(name)" >/dev/null 2>&1; then
    echo "MIG ${MIG} exists — clearing any autohealing policy"
    run gcloud compute instance-groups managed update "${MIG}" \
        --project="${PROJECT}" --zone="${ZONE}" --clear-autohealing
else
    echo "creating MIG ${MIG} at size 0"
    run gcloud compute instance-groups managed create "${MIG}" \
        --project="${PROJECT}" \
        --zone="${ZONE}" \
        --template="${TEMPLATE}" \
        --size=0 \
        --base-instance-name=kythira-kythira-it-mig
fi

# ── 3. Health check and the autohealing group ────────────────────────────────
if gcloud compute health-checks describe "${HEALTH_CHECK}" --project="${PROJECT}" \
    --format="value(name)" >/dev/null 2>&1; then
    echo "health check ${HEALTH_CHECK} exists — leaving it alone"
else
    echo "creating health check ${HEALTH_CHECK}"
    run gcloud compute health-checks create tcp "${HEALTH_CHECK}" \
        --project="${PROJECT}" --port=7000
fi

if gcloud compute instance-groups managed describe "${MIG_AUTOHEAL}" --project="${PROJECT}" \
    --zone="${ZONE}" --format="value(name)" >/dev/null 2>&1; then
    echo "MIG ${MIG_AUTOHEAL} exists — leaving it alone"
else
    echo "creating MIG ${MIG_AUTOHEAL} at size 0, with autohealing"
    run gcloud compute instance-groups managed create "${MIG_AUTOHEAL}" \
        --project="${PROJECT}" \
        --zone="${ZONE}" \
        --template="${TEMPLATE}" \
        --size=0 \
        --base-instance-name=kythira-kythira-it-migah \
        --health-check="${HEALTH_CHECK}" \
        --initial-delay=300
fi

# ── 4. Report, never fix, a group that is not empty ──────────────────────────
if [[ "${DRY_RUN}" -eq 0 ]]; then
    for g in "${MIG}" "${MIG_AUTOHEAL}"; do
        size=$(gcloud compute instance-groups managed describe "${g}" --project="${PROJECT}" \
            --zone="${ZONE}" --format="value(targetSize)")
        if [[ "${size}" != "0" ]]; then
            echo "WARNING: ${g} has target size ${size}. A case left it grown; the" >&2
            echo "  workflow's audit step returns it to 0, or run:" >&2
            echo "  gcloud compute instance-groups managed resize ${g} --size=0 --zone=${ZONE}" >&2
        fi
    done
fi

echo
echo "done. Set the repository variables:"
echo "  gh variable set GCP_TEST_MIG_A        --body \"${MIG}\""
echo "  gh variable set GCP_TEST_MIG_AUTOHEAL --body \"${MIG_AUTOHEAL}\""
echo
echo "The CI service account's roles/compute.instanceAdmin.v1 (gcp-quorum-manager"
echo "bundle) already covers resizing these groups and labelling their instances."
