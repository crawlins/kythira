#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Creates every resource the Azure `quorum-manager` bundle expects an operator
# to have made first, and that `azure_quorum_manager_real_test` cannot make for
# itself:
#
#   * two Virtual Machine Scale Sets, for `azure_vmss_quorum_manager_real`
#   * a Proximity Placement Group, for `placement_proximity_placement_group`
#   * an Availability Set, for `placement_availability_set`
#
# All four are placement *targets*: the manager puts a node into one, it never
# creates one. For the scale sets that is explicit in the design — the model
# (SKU, image, network, zones, upgrade policy) is an out-of-band operator input,
# and `provision_node` only changes `sku.capacity` and tags the instance that
# appears.
#
# Without them the cases do not fail, they *skip*: seven cases across the two
# suites print "Skipping: ... unset" and assert nothing, while the bundle
# reports green off the cases that do run. The five VMSS cases behaved that way
# in every run up to and including 36641739288, and the two placement cases were
# still doing it in 36718807855 — inside the VM half everyone read as covered.
#
# The PPG and the availability set cost nothing to hold. Neither carries a
# charge of its own; only VMs placed in them bill, and the cases delete those.
#
# ## Decisions this script makes, and why
#
# **Flexible orchestration, not Uniform.** This is a hard requirement of the
# manager, not a preference. It identifies each node by a `kythira:node-id` tag
# written onto the instance, and a Uniform scale set's members cannot hold tags
# of their own: they only reflect the scale set's, and every write is accepted,
# reports success, and changes nothing. Measured against a real Uniform set --
# an ARM `PUT`, `az vmss update --instance-id --set tags` and `az resource tag`
# all returned success and applied no tag, still absent 439 seconds later.
# Provisioning would appear to work and every later assess/decommission would
# fail to find the node. A Flexible member is an ordinary
# `Microsoft.Compute/virtualMachines` resource, so it takes tags normally, and
# the manager refuses a Uniform scale set at construction.
#
# `POST .../delete` and `.../deallocate` still work under Flexible; they take
# the member's VM *name* in `instanceIds`, since Flexible has no separate
# numeric instance id. The mode is passed explicitly because `az vmss create`
# has changed its default before.
#
# **Capacity 0 at rest.** An empty scale set costs nothing: there is no hourly
# charge for the scale-set resource itself, only for the instances in it. The
# tests scale it to 1 and back. It also makes `provision_increments_capacity`
# mean what it says.
#
# **No overprovisioning flag.** Overprovisioning is a Uniform-only concept, so
# `--disable-overprovision` is not passed. Under Uniform it was a correctness
# requirement rather than a cost preference: `provision_node` adopts the first
# running instance with no `kythira:node-id` tag, and overprovisioning's
# throwaway instances are exactly that. Flexible never launches them.
#
# **No load balancer and no public IPs.** `az vmss create` otherwise builds a
# Standard Load Balancer, which bills hourly whether or not it carries traffic.
# The tests read the instance's *private* IP off its NIC and never connect to
# it. Public IPs are off by their absence: `--public-ip-per-vm` is a store-true
# flag, so `--public-ip-per-vm false` is not "no public IP", it is
# `unrecognized arguments: false` and a failed create -- measured. `--load-
# balancer ''` does take a value, and the empty string is what suppresses it.
#
# **Regular priority, not Spot.** Spot would be ~80% cheaper, but an eviction
# mid-case would turn `assess_quorum` into a coin flip and produce exactly the
# kind of unattributable flake this suite already has one of. Instances live
# for minutes; the saving is not worth the noise.
#
# **A second, Automatic-mode scale set.** `vmss_rejects_automatic_upgrade_mode`
# asserts the constructor refuses `upgradePolicy.mode=Automatic` (Requirement
# 10.3 — an Automatic scale set replaces instances outside the manager's
# control). Proving a rejection needs a real scale set in that mode; it stays at
# capacity 0 forever and never launches an instance.
#
# Safe to re-run: every step is create-if-absent.
#
# Usage:
#   scripts/ci-cloud-credentials/azure/provision-quorum-manager-fixtures.sh \
#       [--resource-group RG] [--location LOC] [--name NAME] \
#       [--automatic-name NAME] [--vm-size SIZE] [--subnet-id ID] \
#       [--nsg-id ID] [--zone N] [--skip-probe] [--dry-run]
set -euo pipefail

RESOURCE_GROUP="kythira-realtest-rg"
LOCATION="eastus"
NAME="kythira-realtest-vmss"
AUTOMATIC_NAME="kythira-realtest-vmss-auto"
PPG_NAME="kythira-realtest-ppg"
AVSET_NAME="kythira-realtest-avset"
# Standard_D2s_v5 is *restricted* in this subscription in eastus (`az vm
# list-skus` omits it without --all), which is why the VM half of the same
# bundle runs AZURE_TEST_VM_SIZE=Standard_D2s_v7. Default to the same size so
# the two halves fail or succeed on capacity together.
VM_SIZE="Standard_D2s_v7"
SUBNET_ID=""
NSG_ID=""
ZONE="1"
IMAGE="Ubuntu2204"
ADMIN_USER="kythira"
PROBE=1
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: provision-quorum-manager-fixtures.sh [OPTIONS]

Creates (if absent) the two scale sets the azure_vmss_quorum_manager real tier
needs: one in upgradePolicy.mode=Manual that the tests scale up and down, and
one in mode=Automatic that exists only to be rejected. Both at capacity 0.
Safe to re-run.

Optional:
  --resource-group RG    default: kythira-realtest-rg (AZURE_TEST_RESOURCE_GROUP)
  --location LOC         default: eastus (AZURE_TEST_LOCATION)
  --name NAME            default: kythira-realtest-vmss
  --automatic-name NAME  default: kythira-realtest-vmss-auto
  --ppg-name NAME        default: kythira-realtest-ppg
  --avset-name NAME      default: kythira-realtest-avset
  --vm-size SIZE         default: Standard_D2s_v7 — read the header; D2s_v5 is
                          restricted in this subscription
  --subnet-id ID         default: the zone1 subnet of kythira-realtest-vnet in
                          --resource-group
  --nsg-id ID            default: kythira-realtest-nsg in --resource-group
  --zone N               default: 1 — must match the subnet
  --skip-probe           do not scale to 1 and back to prove the set works
  --dry-run              print the calls without running them
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --resource-group) RESOURCE_GROUP="$2"; shift 2 ;;
        --location) LOCATION="$2"; shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        --automatic-name) AUTOMATIC_NAME="$2"; shift 2 ;;
        --ppg-name) PPG_NAME="$2"; shift 2 ;;
        --avset-name) AVSET_NAME="$2"; shift 2 ;;
        --vm-size) VM_SIZE="$2"; shift 2 ;;
        --subnet-id) SUBNET_ID="$2"; shift 2 ;;
        --nsg-id) NSG_ID="$2"; shift 2 ;;
        --zone) ZONE="$2"; shift 2 ;;
        --skip-probe) PROBE=0; shift ;;
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

SUBSCRIPTION="$(az account show --query id -o tsv)"
RG_SCOPE="/subscriptions/${SUBSCRIPTION}/resourceGroups/${RESOURCE_GROUP}"
[[ -n "${SUBNET_ID}" ]] || SUBNET_ID="${RG_SCOPE}/providers/Microsoft.Network/virtualNetworks/kythira-realtest-vnet/subnets/zone${ZONE}"
[[ -n "${NSG_ID}" ]] || NSG_ID="${RG_SCOPE}/providers/Microsoft.Network/networkSecurityGroups/kythira-realtest-nsg"

echo "subscription:   ${SUBSCRIPTION}"
echo "resource group: ${RESOURCE_GROUP} (${LOCATION})"
echo "scale sets:     ${NAME} (Manual), ${AUTOMATIC_NAME} (Automatic)"
echo "vm size:        ${VM_SIZE}  zone=${ZONE}"
echo "subnet:         ${SUBNET_ID}"
echo "placement:      ${PPG_NAME} (PPG), ${AVSET_NAME} (availability set)"

# ── 0. The network the scale sets attach to ──────────────────────────────────
#
# Checked rather than created: the vnet/subnet/NSG are provisioned by whatever
# made AZURE_TEST_SUBNET_ID_ZONE1, and silently creating a second vnet here
# would give CI a scale set on a network the VM half cannot reach.
if [[ "${DRY_RUN}" -eq 0 ]]; then
    az resource show --ids "${SUBNET_ID}" >/dev/null 2>&1 || {
        echo "subnet does not exist: ${SUBNET_ID}" >&2
        echo "It should match the AZURE_TEST_SUBNET_ID_ZONE${ZONE} repository variable." >&2
        exit 1
    }
    az resource show --ids "${NSG_ID}" >/dev/null 2>&1 || {
        echo "network security group does not exist: ${NSG_ID}" >&2
        exit 1
    }
fi

# ── 1. An SSH key for the osProfile ──────────────────────────────────────────
#
# ARM rejects a Linux osProfile carrying neither an SSH key nor an
# adminPassword, so one is required even though nothing ever logs in — the
# tests read power state and a private IP over ARM. Generated fresh and
# discarded: a key nobody keeps cannot be a credential anyone leaks.
SSH_KEY_FILE=""
if [[ "${DRY_RUN}" -eq 0 ]]; then
    SSH_TMP="$(mktemp -d)"
    trap 'rm -rf "${SSH_TMP}"' EXIT
    ssh-keygen -t ed25519 -N '' -C 'kythira-realtest-vmss (unused)' -f "${SSH_TMP}/id" -q
    SSH_KEY_FILE="${SSH_TMP}/id.pub"
fi

# ── 2. The scale sets ────────────────────────────────────────────────────────
create_scale_set() {
    local set_name="$1" upgrade_mode="$2"
    if az vmss show -g "${RESOURCE_GROUP}" -n "${set_name}" >/dev/null 2>&1; then
        local existing_mode existing_orch
        existing_mode="$(az vmss show -g "${RESOURCE_GROUP}" -n "${set_name}" \
            --query upgradePolicy.mode -o tsv)"
        existing_orch="$(az vmss show -g "${RESOURCE_GROUP}" -n "${set_name}" \
            --query orchestrationMode -o tsv)"
        echo "scale set ${set_name} exists (${existing_orch}, upgradePolicy.mode=${existing_mode}) — leaving it alone"
        # Orchestration mode is immutable, so a wrong one cannot be corrected in
        # place: the set has to be deleted and recreated. Loud, and non-zero,
        # because a Uniform set here does not fail -- the manager refuses it at
        # construction, but if that check were ever relaxed the tags would
        # silently not apply. See the header.
        if [[ "${existing_orch}" != "Flexible" ]]; then
            echo "ERROR: ${set_name} uses ${existing_orch} orchestration; this manager requires" >&2
            echo "       Flexible, and the mode cannot be changed in place. Delete it and re-run:" >&2
            echo "         az vmss delete -g ${RESOURCE_GROUP} -n ${set_name}" >&2
            return 1
        fi
        if [[ "${existing_mode}" != "${upgrade_mode}" ]]; then
            echo "WARNING: expected mode ${upgrade_mode}; the tests that depend on it will not" >&2
            echo "         measure what they claim. Delete it and re-run to recreate." >&2
        fi
        return 0
    fi
    echo "creating scale set ${set_name} (mode=${upgrade_mode}, capacity 0)"
    run az vmss create \
        --resource-group "${RESOURCE_GROUP}" \
        --name "${set_name}" \
        --location "${LOCATION}" \
        --orchestration-mode Flexible \
        --instance-count 0 \
        --vm-sku "${VM_SIZE}" \
        --image "${IMAGE}" \
        --admin-username "${ADMIN_USER}" \
        --ssh-key-values "${SSH_KEY_FILE:-/dev/null}" \
        --subnet "${SUBNET_ID}" \
        --nsg "${NSG_ID}" \
        --zones "${ZONE}" \
        --upgrade-policy-mode "${upgrade_mode}" \
        --load-balancer '' \
        --priority Regular \
        --tags 'kythira:fixture=vmss-quorum-manager' \
        -o none
}

# Tagged kythira:fixture, NOT kythira:managed-by: the job's sweep deletes
# anything carrying kythira:managed-by, and these two are operator fixtures that
# must survive every run. The distinct tag is what lets the VMSS audit tell
# "a scale set that should be here" from "instances that should not".
create_scale_set "${NAME}" "Manual"
create_scale_set "${AUTOMATIC_NAME}" "Automatic"

# ── 3. Placement targets for the VM suite ─────────────────────────
#
# `placement_proximity_placement_group` and `placement_availability_set` each
# provision one VM into one of these and assert it comes up live. Both skipped
# silently until now, in the VM half of the bundle that was read as covered.
#
# Neither resource bills. A PPG is a scheduling hint and an availability set is
# a fault/update-domain grouping; both are free to hold empty.
#
# The availability set is created **managed** (aligned). An unmanaged set cannot
# hold VMs with managed disks, which is all this manager creates, and the
# failure surfaces at VM-create time as a mismatch rather than here.
# Tagged after creation, not with `--tags` on the create. `az ppg create` parses
# --tags with the newer shorthand syntax, which reads the colon in the key
# `kythira:fixture` as its own separator and fails with "Shorthand Syntax Error:
# Redundant tail" -- while `az vmss create` above accepts the identical string
# through the classic parser. `az resource tag` takes it either way, so the tag
# goes on in a second call rather than being silently dropped or the key being
# renamed to suit one command's parser.
tag_fixture() {
    run az resource tag --ids "$1" --tags "kythira:fixture=vm-quorum-manager" -o none
}

PPG_ID="${RG_SCOPE}/providers/Microsoft.Compute/proximityPlacementGroups/${PPG_NAME}"
if az ppg show -g "${RESOURCE_GROUP}" -n "${PPG_NAME}" >/dev/null 2>&1; then
    echo "proximity placement group exists — leaving it alone"
else
    echo "creating proximity placement group ${PPG_NAME}"
    run az ppg create --resource-group "${RESOURCE_GROUP}" --name "${PPG_NAME}" \
        --location "${LOCATION}" --type Standard -o none
    tag_fixture "${PPG_ID}"
fi

AVSET_ID="${RG_SCOPE}/providers/Microsoft.Compute/availabilitySets/${AVSET_NAME}"
if az vm availability-set show -g "${RESOURCE_GROUP}" -n "${AVSET_NAME}" >/dev/null 2>&1; then
    echo "availability set exists — leaving it alone"
else
    echo "creating availability set ${AVSET_NAME}"
    # Managed (aligned) by default in this CLI, which is required: an unmanaged
    # set cannot hold the managed-disk VMs this manager creates, and the
    # mismatch would surface at VM-create time rather than here.
    #
    # 2 fault domains: eastus supports 3, but 2 is supported everywhere and
    # nothing here depends on the count. Update domains are irrelevant to a set
    # that holds one VM for the length of one test case.
    run az vm availability-set create --resource-group "${RESOURCE_GROUP}" \
        --name "${AVSET_NAME}" --location "${LOCATION}" \
        --platform-fault-domain-count 2 --platform-update-domain-count 5 -o none
    tag_fixture "${AVSET_ID}"
fi

# ── 4. Prove the set can actually launch an instance ─────────────────────────
#
# Creating a scale set at capacity 0 proves nothing about whether scaling it
# works: quota, SKU restrictions and zone capacity are all only consulted when
# an instance is actually placed. The whole failure this script exists to fix
# was a test that looked green while doing nothing, so the script does not get
# to end on a create call's exit status either.
#
# This is the same sequence provision_node runs: capacity +1, wait for
# PowerState/running, read the private IP off the NIC.
if [[ "${PROBE}" -eq 1 && "${DRY_RUN}" -eq 0 ]]; then
    echo "probing ${NAME}: scaling to 1, waiting for a running instance with a private IP"
    probe_cleanup() {
        echo "probe cleanup: returning ${NAME} to capacity 0"
        az vmss scale -g "${RESOURCE_GROUP}" -n "${NAME}" --new-capacity 0 -o none || {
            echo "FAILED to scale ${NAME} back to 0 — instances may still be BILLING." >&2
            echo "Run: az vmss scale -g ${RESOURCE_GROUP} -n ${NAME} --new-capacity 0" >&2
        }
    }
    trap 'probe_cleanup; rm -rf "${SSH_TMP}"' EXIT

    az vmss scale -g "${RESOURCE_GROUP}" -n "${NAME}" --new-capacity 1 -o none

    # Read through the member's own VM resource, not through the scale set.
    # Under Flexible, `az vmss list-instances --expand instanceView` is refused
    # with "Operation 'VirtualMachineScaleSets.virtualMachines.GET' is not
    # allowed", and `az vmss nic list-vm-nics` 404s -- both address the scale
    # set the Uniform way. `az vm show -d` returns powerState and privateIps
    # together for a member, because a Flexible member is an ordinary VM.
    #
    # The last error is kept and printed. The first version of this loop sent
    # both calls to /dev/null with `|| true`, so when the queries turned out to
    # be Uniform-only it spun silently for ten minutes and then blamed quota --
    # a probe that misreports its own cause is worse than no probe.
    PROBE_IP=""
    PROBE_VM=""
    PROBE_ERR=""
    for _ in $(seq 1 60); do
        if ! MEMBERS="$(az vmss list-instances -g "${RESOURCE_GROUP}" -n "${NAME}" \
                --query '[].instanceId' -o tsv 2>&1)"; then
            PROBE_ERR="listing members failed: ${MEMBERS}"
            sleep 10
            continue
        fi
        PROBE_VM="${MEMBERS%%$'\n'*}"
        if [[ -n "${PROBE_VM}" ]]; then
            # join(), not a bare projection: `--query '[a,b]' -o tsv` prints
            # the two values on separate LINES, not tab-separated columns, so
            # cutting fields off it gave both values for each and the private-IP
            # check passed on the power-state string. One delimited line is
            # unambiguous.
            if ! DETAIL="$(az vm show -d -g "${RESOURCE_GROUP}" -n "${PROBE_VM}" \
                    --query "join('|', [powerState, privateIps])" -o tsv 2>&1)"; then
                PROBE_ERR="reading ${PROBE_VM} failed: ${DETAIL}"
            else
                PROBE_ERR=""
                POWER="${DETAIL%%|*}"
                IP="${DETAIL#*|}"
                if [[ "${POWER}" == *running* && -n "${IP}" && "${IP}" != "None" ]]; then
                    PROBE_IP="${IP}"
                    break
                fi
                PROBE_ERR="${PROBE_VM}: powerState='${POWER}' privateIps='${IP}'"
            fi
        else
            PROBE_ERR="no members listed yet"
        fi
        sleep 10
    done

    if [[ -z "${PROBE_IP}" ]]; then
        echo >&2
        echo "PROBE FAILED: ${NAME} did not produce a running member with a private IP" >&2
        echo "within 10 minutes. provision_node will time out the same way." >&2
        echo "Last observation: ${PROBE_ERR:-none}" >&2
        echo "If that names an az error, the probe is at fault; if it shows a member" >&2
        echo "stuck out of 'running', check quota (az vm list-usage -l ${LOCATION}) and" >&2
        echo "whether ${VM_SIZE} is available in zone ${ZONE} of ${LOCATION}." >&2
        exit 1
    fi
    echo "probe OK: member ${PROBE_VM} running at ${PROBE_IP}"
fi

echo
echo "done. Set the repository variables:"
echo "  AZURE_TEST_VMSS_NAME=${NAME}"
echo "  AZURE_TEST_VMSS_AUTOMATIC_UPGRADE_NAME=${AUTOMATIC_NAME}"
echo "  AZURE_TEST_VMSS_VM_SIZE=${VM_SIZE}"
echo "  AZURE_TEST_PPG_ID=${RG_SCOPE}/providers/Microsoft.Compute/proximityPlacementGroups/${PPG_NAME}"
echo "  AZURE_TEST_AVAILABILITY_SET_ID=${RG_SCOPE}/providers/Microsoft.Compute/availabilitySets/${AVSET_NAME}"
echo
echo "  gh variable set AZURE_TEST_VMSS_NAME --body '${NAME}'"
echo "  gh variable set AZURE_TEST_VMSS_AUTOMATIC_UPGRADE_NAME --body '${AUTOMATIC_NAME}'"
echo "  gh variable set AZURE_TEST_VMSS_VM_SIZE --body '${VM_SIZE}'"
echo "  gh variable set AZURE_TEST_PPG_ID --body '${RG_SCOPE}/providers/Microsoft.Compute/proximityPlacementGroups/${PPG_NAME}'"
echo "  gh variable set AZURE_TEST_AVAILABILITY_SET_ID --body '${RG_SCOPE}/providers/Microsoft.Compute/availabilitySets/${AVSET_NAME}'"
echo
echo "Until all five are set, the Azure job fails closed rather than letting the"
echo "seven cases that need them skip silently."
