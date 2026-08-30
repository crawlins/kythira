#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Capture the machine provenance a cloud benchmark row has to carry
# (.kiro/specs/multi-raft-performance/ Requirement 18.4, extending 6.4) and
# refuse to proceed on a burstable instance type (Requirement 18.5).
#
# Runs ON the instance under measurement. That placement is deliberate and it
# splits the fields into two kinds:
#
#   * Guest-observed  -- vCPU count, CPU model, memory, kernel. Requirement
#     18.4 asks for the CPU "as the guest reports it", because what the
#     hypervisor advertises and what the guest is scheduled on are different
#     claims and only the second one explains a number.
#   * Provider-stated -- network performance, storage class and IOPS. These are
#     control-plane facts (DescribeInstanceTypes / DescribeVolumes), not
#     visible from inside, so the caller passes them in through the
#     KYTHIRA_PERF_STATED_* variables. The workflow already holds credentials
#     for that; the instance deliberately does not need any.
#
# Anything unavailable is emitted as null rather than omitted or guessed. A row
# whose provenance silently lost a field is worse than one that says it does
# not know, because only the second is visible in the artifact.
#
# Usage:  capture-provenance.sh [output.json]     (default: provenance.json)

set -euo pipefail

OUT="${1:-provenance.json}"

# ── IMDSv2 ───────────────────────────────────────────────────────────────────
# Token-first, because IMDSv1 is disabled on any account with
# HttpTokens=required and a v1 probe there hangs until its timeout rather than
# failing -- which would turn "no metadata" into "the job looks stuck".
# Every call is bounded; this script must not be the reason a measured phase
# runs past its ceiling.
IMDS="http://169.254.169.254/latest"
imds_token=""
fetch_token() {
    curl -fsS --max-time 3 -X PUT "$IMDS/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 300" 2>/dev/null || true
}
imds() {
    [ -n "$imds_token" ] || return 1
    curl -fsS --max-time 3 -H "X-aws-ec2-metadata-token: $imds_token" \
        "$IMDS/meta-data/$1" 2>/dev/null || true
}

imds_token="$(fetch_token)"

provider="unknown"
region=""; az=""; instance_type=""; image=""; instance_id=""
if [ -n "$imds_token" ]; then
    provider="aws"
    az="$(imds placement/availability-zone)"
    region="$(imds placement/region)"
    instance_type="$(imds instance-type)"
    image="$(imds ami-id)"
    instance_id="$(imds instance-id)"
fi

# Fallback for providers without an AWS-shaped IMDS, and the seam the burstable
# check is tested through. It only ever *supplies* a type that IMDS did not --
# it cannot override one that IMDS did, so it cannot be used to talk the check
# below out of a refusal on a real instance.
if [ -z "$instance_type" ]; then
    instance_type="${KYTHIRA_PERF_STATED_INSTANCE_TYPE:-}"
fi

# ── Guest-observed facts ─────────────────────────────────────────────────────
# nproc counts what this guest can actually schedule on, which is the number
# that explains a throughput row. /proc/cpuinfo's "model name" is what the
# guest is told it is running on -- on a shared-tenancy instance that is the
# only CPU identity available, and it is what Requirement 18.4 asks to record.
vcpu="$(nproc 2>/dev/null || echo '')"
# `|| true` on the whole assignment, and it is not decoration. This script runs
# under `set -euo pipefail`, and with `pipefail` a pipeline takes the rightmost
# NON-ZERO status -- so a grep that matches nothing kills the script even though
# `sed` at the end of the pipe succeeded. **aarch64 /proc/cpuinfo has no "model
# name" line at all**; it carries "CPU implementer" and "CPU part" instead. So
# the first live Graviton run died here, after a fifty-five minute build, with
# no message: the failing command was a grep whose whole purpose was to be
# allowed to fail.
#
# That is also why the lscpu fallback below could not rescue it. A fallback that
# only runs if the line above *returned* never runs at all.
cpu_model="$(LC_ALL=C /usr/bin/grep -m1 '^model name' /proc/cpuinfo 2>/dev/null \
             | cut -d: -f2- | sed -e 's/^ *//' -e 's/ *$//' || true)"
# aarch64's identity comes from lscpu, which reports "Model name: Neoverse-V1"
# where /proc/cpuinfo reports nothing a human would recognise.
[ -n "$cpu_model" ] || cpu_model="$(lscpu 2>/dev/null \
             | LC_ALL=C /usr/bin/grep -m1 '^Model name' \
             | cut -d: -f2- | sed -e 's/^ *//' -e 's/ *$//' || true)"
# Last resort on a guest whose lscpu is absent too: name the part rather than
# emit null, since aarch64 always carries these two.
if [ -z "$cpu_model" ]; then
    _impl="$(LC_ALL=C /usr/bin/grep -m1 '^CPU implementer' /proc/cpuinfo 2>/dev/null \
             | cut -d: -f2- | tr -d ' ' || true)"
    _part="$(LC_ALL=C /usr/bin/grep -m1 '^CPU part' /proc/cpuinfo 2>/dev/null \
             | cut -d: -f2- | tr -d ' ' || true)"
    if [ -n "$_impl" ] && [ -n "$_part" ]; then
        cpu_model="aarch64 implementer ${_impl} part ${_part}"
    fi
fi
mem_kb="$(LC_ALL=C /usr/bin/grep -m1 '^MemTotal:' /proc/meminfo 2>/dev/null | awk '{print $2}' || true)"
kernel="$(uname -r 2>/dev/null || echo '')"
arch="$(uname -m 2>/dev/null || echo '')"

# ── Tenancy ──────────────────────────────────────────────────────────────────
# Not in IMDS. The caller supplies it if it knows; "shared" is NOT assumed,
# because assuming the common case is exactly how a dedicated-tenancy run
# would end up mislabelled in a comparison table.
tenancy="${KYTHIRA_PERF_STATED_TENANCY:-}"

# ── Provider-stated facts, passed in ─────────────────────────────────────────
net_performance="${KYTHIRA_PERF_STATED_NETWORK:-}"
storage_class="${KYTHIRA_PERF_STATED_STORAGE_CLASS:-}"
storage_iops="${KYTHIRA_PERF_STATED_STORAGE_IOPS:-}"
placement="${KYTHIRA_PERF_STATED_PLACEMENT:-}"

# ── Requirement 18.5: burstable types are refused, not warned about ──────────
# A burstable instance bills CPU credits, and a benchmark that exhausts them
# mid-run reports a credit balance rather than a machine. Matching is on the
# family prefix across the three providers the spec names, and the check is a
# hard failure here rather than a flag on the row: a row that should never be
# published is cheaper to prevent than to explain.
is_burstable() {
    case "$1" in
        t1.*|t2.*|t3.*|t3a.*|t4g.*)            return 0 ;;  # AWS
        e2-micro|e2-small|e2-medium|f1-*)      return 0 ;;  # GCP
        Standard_B*|standard_b*)               return 0 ;;  # Azure
        VM.Standard.E*.Flex)                   return 1 ;;  # OCI flex: not burstable
        *) return 1 ;;
    esac
}

if [ -n "$instance_type" ] && is_burstable "$instance_type"; then
    echo "::error::Refusing to measure on burstable instance type '$instance_type'." >&2
    echo "  A burstable instance bills CPU credits. A benchmark that exhausts them" >&2
    echo "  mid-run measures the credit balance, not the machine." >&2
    echo "  See .kiro/specs/multi-raft-performance/requirements.md Requirement 18.5." >&2
    exit 1
fi

# ── Emit ─────────────────────────────────────────────────────────────────────
# Written with jq so every value is correctly quoted and an empty one becomes
# a real JSON null rather than an empty string -- "" and "unknown" both read
# as data downstream, and null does not.
jqs() { if [ -z "$1" ]; then printf 'null'; else printf '%s' "$1" | jq -R .; fi; }
jqn() { if [ -z "$1" ]; then printf 'null'; else printf '%s' "$1"; fi; }

mem_mib=""
[ -n "$mem_kb" ] && mem_mib=$(( mem_kb / 1024 ))

cat > "$OUT" <<EOF
{
  "schema": "kythira.perf.provenance/1",
  "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "provider": $(jqs "$provider"),
  "region": $(jqs "$region"),
  "availability_zone": $(jqs "$az"),
  "instance_type": $(jqs "$instance_type"),
  "instance_id": $(jqs "$instance_id"),
  "image": $(jqs "$image"),
  "tenancy": $(jqs "$tenancy"),
  "placement": $(jqs "$placement"),
  "guest_observed": {
    "vcpu": $(jqn "$vcpu"),
    "cpu_model": $(jqs "$cpu_model"),
    "memory_mib": $(jqn "$mem_mib"),
    "kernel": $(jqs "$kernel"),
    "arch": $(jqs "$arch")
  },
  "provider_stated": {
    "network_performance": $(jqs "$net_performance"),
    "storage_class": $(jqs "$storage_class"),
    "storage_iops": $(jqn "$storage_iops")
  },
  "burstable": false
}
EOF

jq . "$OUT" > /dev/null || { echo "::error::capture-provenance.sh produced invalid JSON" >&2; exit 1; }
echo "Machine provenance written to $OUT:"
jq . "$OUT"
