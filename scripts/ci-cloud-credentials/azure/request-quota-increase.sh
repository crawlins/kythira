#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# request-quota-increase.sh — raise the Azure Compute vCPU quotas the
# `azure` real-cloud job needs, and report what Microsoft did with the request.
#
# ## Why this exists
#
# `azure_vm_quorum_manager_real_test` has been failing on every scheduled
# `Real Cloud Tests` run since August 24, 2026, on quota rather than on
# anything in the code. Measured on the September 14, 2026 run
# (34841010171), both ceilings were hit in the same test case:
#
#     spot attempt      ARM 409 OperationNotAllowed
#                       LowPriorityCores: limit 3, usage 2, required +2
#     on-demand attempt ARM 409 OperationNotAllowed
#                       Total Regional Cores: limit 10, usage 10, required +2
#
# The manager's spot→on-demand escalation is working correctly: it tried
# spot, was refused, escalated, and was refused again by a second quota. So
# there is nothing to fix in the test — the subscription is simply too small
# for it.
#
# ## Why the defaults are not the minimum
#
# The 409s ask for 4 and 12 respectively. The defaults here are higher on
# purpose: the suite provisions five 2-vCPU VMs per cluster and the
# `zone_outage_during_rolling_deployment` case provisions replacements while
# the originals are still being deleted, so a run's true peak is above its
# steady-state need. Asking for exactly the minimum guarantees a repeat of
# this failure the first time a teardown lags — and each request is a
# round-trip through Microsoft, so the cheap move is to ask once with room.
#
# ## What actually happens when you run this
#
# `az quota update` goes to the Microsoft.Quota resource provider, which
# either applies the new limit immediately or opens a quota request that a
# human at Microsoft reviews. Both outcomes are normal and neither is an
# error. The script prints the request state and, when one was opened, the
# command to poll it. Requests are visible under
# `az quota request status list`.
#
# Safe to re-run: a target at or below the current limit is skipped, never
# submitted, so this can be run to *report* the current state.
#
# Usage:
#   scripts/ci-cloud-credentials/azure/request-quota-increase.sh [--dry-run]
set -euo pipefail

SUBSCRIPTION="${AZURE_CI_SUBSCRIPTION_ID:-65845058-136c-4846-ae74-6b45808544f4}"
LOCATION="${AZURE_TEST_LOCATION:-eastus}"
CORES_TARGET=20
LOW_PRIORITY_TARGET=10
FAMILY=""
FAMILY_TARGET=20
DRY_RUN=0
# Bounds each submit. Microsoft.Quota throttles quota *writes* with a 429
# carrying `Retry-After: 3600`, and the CLI honours that by sleeping for the
# full hour rather than returning -- so an unbounded call is indistinguishable
# from a hang. Measured on 2026-09-22: a second write minutes after a
# successful one was throttled, and `--no-wait` did not help because the 429
# precedes the long-running operation it would have skipped waiting on.
SUBMIT_TIMEOUT=180

usage() {
    cat <<'EOF'
Usage: request-quota-increase.sh [OPTIONS]

Raises the Azure Compute vCPU quotas the azure real-cloud job needs, in the
one region it runs in. Safe to re-run; with no increase to make it simply
reports the current limits.

Optional:
  --subscription ID      default: $AZURE_CI_SUBSCRIPTION_ID, else the
                          repository's CI subscription
  --location LOC         default: $AZURE_TEST_LOCATION, else eastus
  --cores N              Total Regional Cores target (default: 20; the
                          September 14, 2026 failure needed 12)
  --low-priority N       LowPriorityCores target, i.e. spot (default: 10;
                          that failure needed 4)
  --family NAME          also raise a per-VM-family quota, e.g.
                          standardDSv7Family for Standard_D2s_v7. Off by
                          default: the observed 409s cited the two regional
                          quotas, not a family one. Raise it only if a run
                          fails citing the family by name.
  --family-target N      target for --family (default: 20)
  --submit-timeout N     seconds to allow each submit (default: 180). A
                          throttled write returns 429 with Retry-After 3600
                          and the CLI sleeps it off, so an unbounded call
                          looks like a hang; this bounds it and says so.
  --dry-run              print the calls without submitting them
  -h, --help             this message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --subscription) SUBSCRIPTION="$2"; shift 2 ;;
        --location) LOCATION="$2"; shift 2 ;;
        --cores) CORES_TARGET="$2"; shift 2 ;;
        --low-priority) LOW_PRIORITY_TARGET="$2"; shift 2 ;;
        --family) FAMILY="$2"; shift 2 ;;
        --family-target) FAMILY_TARGET="$2"; shift 2 ;;
        --submit-timeout) SUBMIT_TIMEOUT="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

for n in "${CORES_TARGET}" "${LOW_PRIORITY_TARGET}" "${FAMILY_TARGET}"; do
    [[ "${n}" =~ ^[0-9]+$ ]] || { echo "ERROR: targets must be whole numbers, got '${n}'" >&2; exit 1; }
done

# ── Preflight ────────────────────────────────────────────────────────────────
# Asserted rather than assumed, in the order that produces the most useful
# message: a missing extension and an expired login fail very differently.
command -v az >/dev/null 2>&1 || {
    echo "ERROR: the Azure CLI is not on PATH." >&2
    exit 1
}
if ! az extension show --name quota >/dev/null 2>&1; then
    echo "ERROR: the 'quota' CLI extension is not installed. Install it with:" >&2
    echo "         az extension add --name quota" >&2
    exit 1
fi
az account show >/dev/null 2>&1 || {
    echo "ERROR: not logged in. Run 'az login' first." >&2
    exit 1
}

SCOPE="/subscriptions/${SUBSCRIPTION}/providers/Microsoft.Compute/locations/${LOCATION}"

echo "subscription: ${SUBSCRIPTION}"
echo "location:     ${LOCATION}"
echo

# Reads the quota object as it exists and echoes "<limit> <resourceType>".
# The resourceType is taken from the live object rather than hardcoded:
# 'cores' and 'lowPriorityCores' do not carry the same one, and an update
# that sends the wrong type is rejected in a way that reads like a
# permissions problem. Letting Azure tell us avoids the guess entirely.
read_quota() {
    local name="$1"
    az quota show --resource-name "${name}" --scope "${SCOPE}" -o json 2>/dev/null \
        | python3 -c '
import json, sys
try:
    p = json.load(sys.stdin)["properties"]
except Exception:
    sys.exit(1)
limit = p.get("limit", {})
value = limit.get("value", limit) if isinstance(limit, dict) else limit
print(value, p.get("resourceType", ""))
' 2>/dev/null || return 1
}

# Returns 0 if it submitted, 1 if it skipped. Never lowers a limit: a target
# at or below the current value is a no-op, so an accidental small --cores
# cannot shrink the subscription.
request_increase() {
    local name="$1" target="$2" label="$3"

    local current res_type
    if ! read -r current res_type < <(read_quota "${name}"); then
        echo "  ${label} (${name}): could not read the current quota."
        echo "    The name may not exist in this region, or the signed-in"
        echo "    principal may not be able to read Microsoft.Quota here."
        return 1
    fi

    if [[ ! "${current}" =~ ^[0-9]+$ ]]; then
        echo "  ${label} (${name}): unexpected current limit '${current}' — skipping."
        return 1
    fi

    if (( current >= target )); then
        echo "  ${label} (${name}): limit is already ${current} (>= ${target}) — nothing to do."
        return 1
    fi

    echo "  ${label} (${name}): ${current} -> ${target}"

    local -a cmd=(az quota update
        --resource-name "${name}"
        --scope "${SCOPE}"
        --limit-object "value=${target}")
    [[ -n "${res_type}" ]] && cmd+=(--resource-type "${res_type}")

    if [[ "${DRY_RUN}" == "1" ]]; then
        printf '    [dry-run]'; printf ' %q' "${cmd[@]}"; echo
        return 0
    fi

    # A rejected request is not a script failure: Microsoft declines quota
    # for reasons this script cannot see (payment method, region capacity,
    # account age). Report it and carry on to the next quota.
    local out rc=0
    out="$(timeout "${SUBMIT_TIMEOUT}" "${cmd[@]}" -o json 2>&1)" || rc=$?
    if (( rc == 124 )); then
        cat <<EOF
    NO RESPONSE within ${SUBMIT_TIMEOUT}s — almost certainly throttled.
      Microsoft.Quota answers a too-frequent quota write with
        429 RequestThrottled "please retry after 3600 seconds"
      and the CLI obeys that Retry-After by sleeping, so a throttled call
      looks like a hang rather than a refusal. Nothing was submitted; the
      limit is unchanged. Wait an hour and re-run — already-satisfied
      quotas are skipped, so a re-run only retries what is still short.
EOF
        return 1
    fi
    if (( rc != 0 )); then
        echo "    REQUEST FAILED:"
        printf '      %s\n' "${out}" | head -20
        return 1
    fi
    printf '%s' "${out}" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("    submitted (the response was not JSON; check the portal)")
    sys.exit(0)
p = d.get("properties", d)
state = p.get("provisioningState") or p.get("requestSubmitTime") or "submitted"
print(f"    state: {state}")
if d.get("id"):
    print(f"    request: {d['id']}")
' 2>/dev/null || echo "    submitted"
    return 0
}

echo "Current limits and requested targets:"
submitted=0
request_increase cores "${CORES_TARGET}" "Total Regional Cores" && submitted=1
request_increase lowPriorityCores "${LOW_PRIORITY_TARGET}" "LowPriorityCores (spot)" && submitted=1
if [[ -n "${FAMILY}" ]]; then
    request_increase "${FAMILY}" "${FAMILY_TARGET}" "VM family" && submitted=1
fi

echo
if [[ "${DRY_RUN}" == "1" ]]; then
    echo "Dry run: nothing was submitted."
elif [[ "${submitted}" == "1" ]]; then
    cat <<EOF
Submitted. Microsoft either applied these immediately or opened a request a
human reviews; both are normal. Poll with:

  az quota request status list --scope '${SCOPE}' -o table

The azure real-cloud job stays red until the new limits are live. Re-run this
script with no arguments to see the current limits at any time.
EOF
else
    echo "Nothing was submitted — every quota already meets its target."
fi
